#include "renderer/vulkan/vk_command_buffer.h"

#include "renderer/vulkan/texture_cache.h"
#include "renderer/vulkan/vk_device.h"
#include "renderer/vulkan/vk_pipeline.h"

#include <unistd.h>
#include <limits.h>

#include <glm/gtc/matrix_transform.hpp>
#include <unistd.h>
#include "renderer/vulkan/vk_pipeline.h"

#include <unistd.h>
#include <limits.h>
#include "renderer/vulkan/vk_swapchain.h"

#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>

namespace engine {

namespace {

constexpr uint32_t kFramesInFlight = 2;

// Matches shader CameraUBO (std140-ish): 4 mat4s + 5 vec4s = 272 bytes.
struct FrameUBO {
    glm::mat4 viewProjection;
    glm::vec4 cameraPos;
    glm::vec4 lightDir;   // normalized direction light travels
    glm::vec4 lightColor; // rgb
    glm::vec4 params;     // x = ambient
    glm::mat4 lightVP[VulkanShadowPass::kCascadeCount];
    glm::vec4 cascadeSplits; // xyzw = view-space far distances of cascades 0..3
};
constexpr VkDeviceSize kCameraUboSize = sizeof(FrameUBO);

// 1x1 white fallback texture data.
const unsigned char whitePixel[4] = {255, 255, 255, 255};

void fatal(const char* msg) {
    std::fprintf(stderr, "Fatal: %s\n", msg);
    std::exit(EXIT_FAILURE);
}

} // namespace

VulkanCommandBuffer::VulkanCommandBuffer(VulkanDevice& device,
                                         VulkanSwapchain& swapchain,
                                         const VulkanPipeline& pipeline,
                                         TextureCache& textures)
    : device_(device), swapchain_(swapchain), pipeline_(pipeline),
      textures_(textures) {
    shadowPass_ = new VulkanShadowPass(device_, swapchain_);
    environment_ = new VulkanEnvironment(device_, swapchain_,
                                         device_.cameraDescriptorLayout(),
                                         device_.materialDescriptorLayout(),
                                         device_.shadowSamplerLayout());
    createComputeResources();
    createCullPipeline();

    // Allocate per-frame compute descriptor sets.
    {
        VkDevice dev = device_.handle();
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkDescriptorSetAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool = cullPool_;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &cullSetLayout_;
            if (vkAllocateDescriptorSets(dev, &ai, &cullSets_[i]) != VK_SUCCESS)
                fatal("cull set alloc");
        }
    }

    device_.createFrameFences(kFramesInFlight);
    device_.createUniformBuffers(kFramesInFlight, kCameraUboSize);
    buffers_.resize(kFramesInFlight);
    for (auto& cmd : buffers_) cmd = device_.allocateCommandBuffer();

    createCameraDescriptors();
    createShadowSamplerSets();
    createIndirectBuffer();
}

VulkanCommandBuffer::~VulkanCommandBuffer() {
    delete shadowPass_;
    delete environment_;
    VkDevice dev = device_.handle();
    if (cameraDescriptorPool_ != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(dev, cameraDescriptorPool_, nullptr);
    if (materialPool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(dev, materialPool_, nullptr);
    if (shadowSamplerPool_ != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(dev, shadowSamplerPool_, nullptr);
    if (indirectBuffer_) {
        vkUnmapMemory(dev, indirectMemory_);
        vkDestroyBuffer(dev, indirectBuffer_, nullptr);
        vkFreeMemory(dev, indirectMemory_, nullptr);
    }
}

void VulkanCommandBuffer::createIndirectBuffer() {
    VkDevice dev = device_.handle();
    VkDeviceSize bytes = kMaxIndirectDraws * sizeof(VkDrawIndexedIndirectCommand);

    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (vkCreateBuffer(dev, &bi, nullptr, &indirectBuffer_) != VK_SUCCESS)
        fatal("indirect buffer create");
    VkMemoryRequirements reqs{};
    vkGetBufferMemoryRequirements(dev, indirectBuffer_, &reqs);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(device_.physical(), &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((reqs.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            VkMemoryAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize = reqs.size;
            ai.memoryTypeIndex = i;
            if (vkAllocateMemory(dev, &ai, nullptr, &indirectMemory_) == VK_SUCCESS)
                break;
            indirectMemory_ = VK_NULL_HANDLE;
        }
    }
    if (!indirectMemory_) fatal("indirect memory alloc");
    vkBindBufferMemory(dev, indirectBuffer_, indirectMemory_, 0);
    if (vkMapMemory(dev, indirectMemory_, 0, bytes, 0, &indirectMapped_) != VK_SUCCESS)
        fatal("indirect map");
}

namespace {

std::string cbExeDir() {
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return ".";
    std::string p(buf, static_cast<size_t>(len));
    auto slash = p.find_last_of('/');
    return slash == std::string::npos ? "." : p.substr(0, slash);
}

std::vector<char> cbReadFileBytes(const std::string& path) {
    std::string exeDirVal = cbExeDir();
    std::string full = path;
    for (const std::string& base : {exeDirVal + "/", exeDirVal + "/../"}) {
        FILE* probe = std::fopen((base + path).c_str(), "rb");
        if (probe) { std::fclose(probe); full = base + path; break; }
    }
    FILE* f = std::fopen(full.c_str(), "rb");
    if (!f) {
        char buf[256];
        snprintf(buf, sizeof(buf), "failed to open shader: %s", full.c_str());
        fatal(buf);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    std::vector<char> bytes(static_cast<size_t>(size));
    if (fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) {
        fclose(f);
        fatal("short read");
    }
    fclose(f);
    return bytes;
}

VkShaderModule cbCreateShaderModule(VkDevice device, const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &ci, nullptr, &m) != VK_SUCCESS)
        fatal("shader module create");
    return m;
}

} // anonymous namespace

void VulkanCommandBuffer::createComputeResources() {
    VkDevice dev = device_.handle();
    VkDeviceSize instanceBytes =
        4096 * sizeof(engine::InstanceData); // max instances

    // Input instances: host-visible for CPU upload.
    {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = instanceBytes;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(dev, &bi, nullptr, &instanceInBuffer_) != VK_SUCCESS)
            fatal("instance in buffer");
        VkMemoryRequirements reqs{};
        vkGetBufferMemoryRequirements(dev, instanceInBuffer_, &reqs);
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(device_.physical(), &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
            if ((reqs.memoryTypeBits & (1u << i)) &&
                (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                VkMemoryAllocateInfo ai{};
                ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                ai.allocationSize = reqs.size;
                ai.memoryTypeIndex = i;
                if (vkAllocateMemory(dev, &ai, nullptr, &instanceInMemory_) == VK_SUCCESS)
                    break;
                instanceInMemory_ = VK_NULL_HANDLE;
            }
        }
        if (!instanceInMemory_) fatal("instance in mem");
        vkBindBufferMemory(dev, instanceInBuffer_, instanceInMemory_, 0);
        vkMapMemory(dev, instanceInMemory_, 0, instanceBytes, 0, &instanceInMapped_);
    }

    // Output instances: device-local (compute writes, graphics reads).
    {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = instanceBytes;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if (vkCreateBuffer(dev, &bi, nullptr, &instanceOutBuffer_) != VK_SUCCESS)
            fatal("instance out buffer");
        VkMemoryRequirements reqs{};
        vkGetBufferMemoryRequirements(dev, instanceOutBuffer_, &reqs);
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(device_.physical(), &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
            if ((reqs.memoryTypeBits & (1u << i)) &&
                (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                VkMemoryAllocateInfo ai{};
                ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                ai.allocationSize = reqs.size;
                ai.memoryTypeIndex = i;
                if (vkAllocateMemory(dev, &ai, nullptr, &instanceOutMemory_) == VK_SUCCESS)
                    break;
                instanceOutMemory_ = VK_NULL_HANDLE;
            }
        }
        if (!instanceOutMemory_) fatal("instance out mem");
        vkBindBufferMemory(dev, instanceOutBuffer_, instanceOutMemory_, 0);
    }

    // Compute descriptor pool + sets.
    VkDescriptorPoolSize sizes[3]{};
    sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sizes[0].descriptorCount = kFramesInFlight * 2;
    sizes[1] = sizes[0];
    sizes[2] = sizes[0];

    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = kFramesInFlight;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &cullPool_) != VK_SUCCESS)
        fatal("cull pool");

    cullSets_.resize(kFramesInFlight);
}

void VulkanCommandBuffer::createCullPipeline() {
    VkDevice dev = device_.handle();

    auto code = cbReadFileBytes("shaders/cull.comp.spv");
    VkShaderModule module = cbCreateShaderModule(dev, code);

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = "main";

    // Push constants: VP matrix + frustum planes + counts
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(glm::mat4) + 6 * sizeof(glm::vec4) + 2 * sizeof(uint32_t);

    // Set 0 layout: 3 storage buffer bindings (input, output, indirect).
    VkDescriptorSetLayoutBinding bindings[3]{};
    for (int i = 0; i < 3; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 3;
    layoutInfo.pBindings = bindings;

    if (vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr, &cullSetLayout_) != VK_SUCCESS)
        fatal("cull set layout");

    VkPipelineLayoutCreateInfo pipeLayoutInfo{};
    pipeLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeLayoutInfo.setLayoutCount = 1;
    pipeLayoutInfo.pSetLayouts = &cullSetLayout_;
    pipeLayoutInfo.pushConstantRangeCount = 1;
    pipeLayoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(dev, &pipeLayoutInfo, nullptr, &cullLayout_) != VK_SUCCESS)
        fatal("cull pipeline layout");

    VkComputePipelineCreateInfo pipeInfo{};
    pipeInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeInfo.stage = stage;
    pipeInfo.layout = cullLayout_;
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pipeInfo, nullptr,
                                 &cullPipeline_) != VK_SUCCESS)
        fatal("cull pipeline create");

    vkDestroyShaderModule(dev, module, nullptr);
}

void VulkanCommandBuffer::createCameraDescriptors() {
    VkDevice dev = device_.handle();

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = kFramesInFlight;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kFramesInFlight;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(dev, &poolInfo, nullptr, &cameraDescriptorPool_) != VK_SUCCESS)
        fatal("failed to create camera descriptor pool");

    descriptorSets_.resize(kFramesInFlight);
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = cameraDescriptorPool_;
        ai.descriptorSetCount = 1;
        VkDescriptorSetLayout camL = pipeline_.cameraLayout();
        ai.pSetLayouts = &camL;

        if (vkAllocateDescriptorSets(dev, &ai, &descriptorSets_[i]) != VK_SUCCESS)
            fatal("failed to allocate camera descriptor set");

        VkDescriptorBufferInfo bufInfo{};
        bufInfo.buffer = device_.uniformBuffer(i);
        bufInfo.offset = 0;
        bufInfo.range = kCameraUboSize;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptorSets_[i];
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo = &bufInfo;
        vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);
    }
}

void VulkanCommandBuffer::createShadowSamplerSets() {
    VkDevice dev = device_.handle();
    VkDescriptorSetLayout layout = device_.shadowSamplerLayout();

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount =
        VulkanShadowPass::kCascadeCount * kFramesInFlight; // 4 per set × 2 sets

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kFramesInFlight;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(dev, &poolInfo, nullptr, &shadowSamplerPool_) != VK_SUCCESS)
        fatal("failed to create shadow sampler pool");

    shadowSamplerSets_.resize(kFramesInFlight);
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = shadowSamplerPool_;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &layout;
        if (vkAllocateDescriptorSets(dev, &allocInfo, &shadowSamplerSets_[i]) != VK_SUCCESS)
            fatal("failed to allocate shadow sampler set");

        std::array<VkDescriptorImageInfo, VulkanShadowPass::kCascadeCount> infos{};
        for (uint32_t c = 0; c < VulkanShadowPass::kCascadeCount; ++c) {
            infos[c].sampler = shadowPass_->sampler();
            infos[c].imageView = shadowPass_->view(c);
            infos[c].imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
        }
        std::array<VkWriteDescriptorSet, VulkanShadowPass::kCascadeCount> writes{};
        for (uint32_t c = 0; c < VulkanShadowPass::kCascadeCount; ++c) {
            writes[c].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[c].dstSet = shadowSamplerSets_[i];
            writes[c].dstBinding = c;
            writes[c].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[c].descriptorCount = 1;
            writes[c].pImageInfo = &infos[c];
        }
        vkUpdateDescriptorSets(dev, static_cast<uint32_t>(writes.size()), writes.data(),
                               0, nullptr);
    }
}

VkDescriptorSet VulkanCommandBuffer::materialDescriptor(const Texture* tex) {
    if (!tex) tex = textures_.createFromPixels("default_white", whitePixel, 1, 1);

    auto it = materialSets_.find(tex);
    if (it != materialSets_.end()) return it->second;

    VkDevice dev = device_.handle();

    // Create the pool on first use.
    if (materialPool_ == VK_NULL_HANDLE) {
        VkDescriptorPoolSize ps{};
        ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ps.descriptorCount = 64;
        VkDescriptorPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.maxSets = 64;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &ps;
        if (vkCreateDescriptorPool(dev, &pi, nullptr, &materialPool_) != VK_SUCCESS)
            fatal("failed to create material descriptor pool");
    }

    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = materialPool_;
    allocInfo.descriptorSetCount = 1;
    VkDescriptorSetLayout matLayout = device_.materialDescriptorLayout();
    allocInfo.pSetLayouts = &matLayout;
    if (vkAllocateDescriptorSets(dev, &allocInfo, &set) != VK_SUCCESS)
        fatal("failed to allocate material descriptor set");

    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = tex->sampler;
    imageInfo.imageView = tex->view;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);

    materialSets_[tex] = set;
    return set;
}

void VulkanCommandBuffer::recordFrame(uint32_t frameIndex, uint32_t imageIndex,
                                      const std::vector<PendingBatch>& batches,
                                      const glm::mat4& viewProjection,
                                      const DirectionalLight& light,
                                      const glm::vec3& cameraPos) {
    // --- CPU: compute cascades + fill UBO ---------------------------------
    VkCommandBuffer cmd = buffers_[frameIndex];
    const float camNear = 0.1f;
    const float camFar = 150.0f;
    float splitsNorm[4] = {0.05f, 0.15f, 0.35f, 1.0f};
    glm::vec3 dir = glm::normalize(light.direction);

    FrameUBO ubo{};
    ubo.viewProjection = viewProjection;
    ubo.cameraPos = glm::vec4(cameraPos, 1.0f);
    ubo.lightDir = glm::vec4(dir, 0.0f);
    ubo.lightColor = glm::vec4(light.color, 1.0f);
    ubo.params = glm::vec4(0.25f, 0.0f, 0.0f, 0.0f);

    auto ndcZ = [&](float d) {
        return ((camFar + camNear) * d - 2.0f * camFar * camNear) /
               ((camFar - camNear) * d);
    };
    glm::mat4 invVP = glm::inverse(viewProjection);

    for (uint32_t c = 0; c < VulkanShadowPass::kCascadeCount; ++c) {
        float farDist = camNear + (camFar - camNear) * splitsNorm[c];
        float prevDist = c == 0 ? camNear : camNear + (camFar - camNear) * splitsNorm[c - 1];
        float zn = ndcZ(prevDist), zf = ndcZ(farDist);

        glm::vec3 corners[8];
        uint32_t n = 0;
        for (uint32_t zi = 0; zi < 2; ++zi) {
            float z = zi == 0 ? zn : zf;
            for (float sx : {-1.0f, 1.0f})
                for (float sy : {-1.0f, 1.0f}) {
                    glm::vec4 wh = invVP * glm::vec4(sx, sy, z, 1.0f);
                    corners[n++] = glm::vec3(wh) / wh.w;
                }
        }

        glm::vec3 center{0.0f};
        for (const auto& corner : corners) center += corner;
        center /= 8.0f;

        glm::mat4 lightView =
            glm::lookAt(center - dir * 40.0f, center, glm::vec3(0.0f, 1.0f, 0.0f));

        float minX = FLT_MAX, maxX = -FLT_MAX, minY = FLT_MAX, maxY = -FLT_MAX,
              minZ = FLT_MAX, maxZ = -FLT_MAX;
        for (const auto& corner : corners) {
            glm::vec3 ls = lightView * glm::vec4(corner, 1.0f);
            minX = std::min(minX, ls.x); maxX = std::max(maxX, ls.x);
            minY = std::min(minY, ls.y); maxY = std::max(maxY, ls.y);
            minZ = std::min(minZ, ls.z); maxZ = std::max(maxZ, ls.z);
        }
        float texelX = (maxX - minX) / static_cast<float>(VulkanShadowPass::kSize);
        if (texelX > 0) { minX = std::floor(minX / texelX) * texelX; maxX = std::ceil(maxX / texelX) * texelX; }
        float texelY = (maxY - minY) / static_cast<float>(VulkanShadowPass::kSize);
        if (texelY > 0) { minY = std::floor(minY / texelY) * texelY; maxY = std::ceil(maxY / texelY) * texelY; }

        glm::mat4 lightProj = glm::ortho(minX, maxX, minY, maxY, -maxZ - 10.0f,
                                         -minZ + 10.0f);
        ubo.lightVP[c] = lightProj * lightView;
    }
    ubo.cascadeSplits = glm::vec4(splitsNorm[1], splitsNorm[2], splitsNorm[3], camFar);

    // --- Upload ALL instances to input SSBO (no CPU culling) --------------
    uint32_t totalInstances = 0;
    {
        auto* dst = reinterpret_cast<engine::InstanceData*>(instanceInMapped_);
        for (const auto& batch : batches) {
            const auto& instances = *batch.instances;
            for (size_t i = 0; i < instances.size() && totalInstances < 4096; ++i)
                dst[totalInstances++] = instances[i];
        }
    }

    // Reset indirect commands to zero.
    std::memset(indirectMapped_, 0, kMaxIndirectDraws * sizeof(VkDrawIndexedIndirectCommand));

    // Upload UBO.
    std::memcpy(device_.mapUniform(frameIndex), &ubo, sizeof(FrameUBO));

    // --- GPU recording ----------------------------------------------------
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS)
        fatal("failed to begin recording command buffer");

    // Shadow passes: one per cascade.
    for (uint32_t c = 0; c < VulkanShadowPass::kCascadeCount; ++c) {
        shadowPass_->begin(cmd, c);
        for (const auto& batch : batches)
            shadowPass_->drawBatch(cmd, *batch.mesh, *batch.instances,
                                   ubo.lightVP[c]);
        shadowPass_->end(cmd);
    }

    // Main render pass.
    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = {{0.02f, 0.02f, 0.05f, 1.0f}};
    clearValues[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo passInfo{};
    passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    passInfo.renderPass = swapchain_.renderPass();
    passInfo.framebuffer = swapchain_.framebuffer(imageIndex);
    passInfo.renderArea.offset = {0, 0};
    passInfo.renderArea.extent = swapchain_.extent();
    passInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    passInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(cmd, &passInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());

    // Bind sets 0 (camera), 2 (shadow), 3 (env). Set 1 bound per batch below.
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.layout(),
                            0, 1, &descriptorSets_[frameIndex], 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.layout(),
                            2, 1, &shadowSamplerSets_[frameIndex], 0, nullptr);

    VkViewport viewport{};
    viewport.width = static_cast<float>(swapchain_.extent().width);
    viewport.height = static_cast<float>(swapchain_.extent().height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{{0, 0}, swapchain_.extent()};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Fill indirect commands from CPU (host-visible buffer).
    auto* cmds = reinterpret_cast<VkDrawIndexedIndirectCommand*>(indirectMapped_);
    uint32_t drawCount = 0;

    for (const auto& batch : batches) {
        const Mesh& mesh = *batch.mesh;
        const auto& instances = *batch.instances;
        if (instances.empty() || mesh.indices.empty() || mesh.vertices.empty()) continue;
        if (drawCount >= kMaxIndirectDraws) break;

        cmds[drawCount].indexCount = static_cast<uint32_t>(mesh.indices.size());
        cmds[drawCount].instanceCount = static_cast<uint32_t>(instances.size());
        cmds[drawCount].firstIndex = 0;
        cmds[drawCount].vertexOffset = 0;
        cmds[drawCount].firstInstance = 0;
        ++drawCount;

        VkBuffer vertexBuffer = device_.scratchVertexBuffer(mesh.vertices);
        VkBuffer instanceBuffer = device_.scratchVertexBuffer(instances);
        VkBuffer buffers[] = {vertexBuffer, instanceBuffer};
        VkDeviceSize offsets[] = {0, 0};
        vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);

        VkBuffer indexBuffer = device_.scratchIndexBuffer(mesh.indices);
        vkCmdBindIndexBuffer(cmd, indexBuffer, 0, VK_INDEX_TYPE_UINT32);

        VkDescriptorSet matSet = materialDescriptor(batch.texture);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline_.layout(), 1, 1, &matSet, 0, nullptr);

        vkCmdDrawIndexedIndirect(cmd, indirectBuffer_,
                                 drawCount * sizeof(VkDrawIndexedIndirectCommand),
                                 1, sizeof(VkDrawIndexedIndirectCommand));
    }

    vkCmdEndRenderPass(cmd);

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
        fatal("failed to end recording command buffer");
}
} // namespace engine
