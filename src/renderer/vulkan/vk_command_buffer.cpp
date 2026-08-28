#include "renderer/vulkan/vk_command_buffer.h"

#include "renderer/vulkan/gpu_occlusion.h"
#include "renderer/vulkan/shadow_pass.h"
#include "renderer/vulkan/environment.h"
#include "renderer/vulkan/texture_cache.h"
#include "renderer/vulkan/vk_device.h"
#include "renderer/vulkan/vk_pipeline.h"
#include "renderer/vulkan/vk_swapchain.h"

#include <unistd.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <glm/gtc/matrix_transform.hpp>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace engine {

namespace {

void fatal(const char* msg) {
    std::fprintf(stderr, "Fatal: %s\n", msg);
    std::exit(EXIT_FAILURE);
}

constexpr uint32_t kFramesInFlight = 2;
constexpr VkDeviceSize kCameraUboSize = sizeof(glm::mat4) * 6 + sizeof(glm::vec4) * 2; // ~400B
const unsigned char whitePixel[4] = {255, 255, 255, 255};

std::vector<char> cbReadFileBytes(const std::string& path) {
    std::ifstream f(path, std::ios::ate | std::ios::binary);
    if (!f) fatal((std::string("open shader: ") + path).c_str());
    size_t size = static_cast<size_t>(f.tellg());
    std::vector<char> bytes(size);
    f.seekg(0);
    f.read(bytes.data(), size);
    return bytes;
}

VkShaderModule cbShaderModule(VkDevice dev, const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &ci, nullptr, &m) != VK_SUCCESS)
        fatal("shader module create");
    return m;
}

} // namespace

VulkanCommandBuffer::VulkanCommandBuffer(VulkanDevice& device,
                                         VulkanSwapchain& swapchain,
                                         const VulkanPipeline& pipeline,
                                         TextureCache& textures)
    : device_(device), swapchain_(swapchain), pipeline_(pipeline),
      textures_(textures) {
    shadowPass_ = new VulkanShadowPass(device_, swapchain_);
    occlusion_ = new VulkanGpuOcclusion(device_, swapchain_);
    // One primary command buffer per frame in flight.
    buffers_.resize(kFramesInFlight);
    for (uint32_t i = 0; i < kFramesInFlight; ++i)
        buffers_[i] = device_.allocateCommandBuffer();
    environment_ = new VulkanEnvironment(device_, swapchain_,
                                         device_.cameraDescriptorLayout(),
                                         device_.materialDescriptorLayout(),
                                         device_.shadowSamplerLayout());
    createIndirectBuffer();
    createInstanceBuffers();
    createCameraDescriptors();
    createShadowSamplerSets();
    createCullPipeline();
    createHizPipeline();
}

VulkanCommandBuffer::~VulkanCommandBuffer() {
    delete shadowPass_;
    delete environment_;
    delete occlusion_;
    VkDevice dev = device_.handle();
    if (indirectBuffer_) {
        vkUnmapMemory(dev, indirectMemory_);
        vkDestroyBuffer(dev, indirectBuffer_, nullptr);
        vkFreeMemory(dev, indirectMemory_, nullptr);
    }
    if (cameraDescriptorPool_) vkDestroyDescriptorPool(dev, cameraDescriptorPool_, nullptr);
    if (materialPool_) vkDestroyDescriptorPool(dev, materialPool_, nullptr);
    if (shadowSamplerPool_) vkDestroyDescriptorPool(dev, shadowSamplerPool_, nullptr);
    if (cullPipeline_) vkDestroyPipeline(dev, cullPipeline_, nullptr);
    if (cullLayout_) vkDestroyPipelineLayout(dev, cullLayout_, nullptr);
    if (cullSetLayout_) vkDestroyDescriptorSetLayout(dev, cullSetLayout_, nullptr);
    if (cullPool_) vkDestroyDescriptorPool(dev, cullPool_, nullptr);
    if (hizPipeline_) vkDestroyPipeline(dev, hizPipeline_, nullptr);
    if (hizLayout_) vkDestroyPipelineLayout(dev, hizLayout_, nullptr);
    if (hizSetLayout_) vkDestroyDescriptorSetLayout(dev, hizSetLayout_, nullptr);
    if (hizPool_) vkDestroyDescriptorPool(dev, hizPool_, nullptr);
    if (instanceInBuffer_) {
        vkUnmapMemory(dev, instanceInMemory_);
        vkDestroyBuffer(dev, instanceInBuffer_, nullptr);
        vkFreeMemory(dev, instanceInMemory_, nullptr);
    }
    if (instanceOutBuffer_) {
        vkDestroyBuffer(dev, instanceOutBuffer_, nullptr);
        vkFreeMemory(dev, instanceOutMemory_, nullptr);
    }
    if (batchRangeBuffer_) {
        vkUnmapMemory(dev, batchRangeMemory_);
        vkDestroyBuffer(dev, batchRangeBuffer_, nullptr);
        vkFreeMemory(dev, batchRangeMemory_, nullptr);
    }
}

void VulkanCommandBuffer::createIndirectBuffer() {
    VkDevice dev = device_.handle();
    VkDeviceSize bytes = kMaxIndirectDraws * sizeof(VkDrawIndexedIndirectCommand);

    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bi.size = bytes;
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
            if (vkAllocateMemory(dev, &ai, nullptr, &indirectMemory_) == VK_SUCCESS) break;
            indirectMemory_ = VK_NULL_HANDLE;
        }
    }
    if (!indirectMemory_) fatal("indirect memory alloc");
    vkBindBufferMemory(dev, indirectBuffer_, indirectMemory_, 0);
    if (vkMapMemory(dev, indirectMemory_, 0, bytes, 0, &indirectMapped_) != VK_SUCCESS)
        fatal("indirect map");
}

namespace {
// Host-visible buffer helper (shared by instance input + batch ranges).
void cbMakeHostVisibleBuffer(VulkanDevice& device, VkDeviceSize bytes,
                             VkBufferUsageFlags usage, VkBuffer* buffer,
                             VkDeviceMemory* memory, void** mapped) {
    VkDevice dev = device.handle();
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = bytes;
    bi.usage = usage;
    if (vkCreateBuffer(dev, &bi, nullptr, buffer) != VK_SUCCESS)
        fatal("host-visible buffer create");
    VkMemoryRequirements reqs{};
    vkGetBufferMemoryRequirements(dev, *buffer, &reqs);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(device.physical(), &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((reqs.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            VkMemoryAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize = reqs.size;
            ai.memoryTypeIndex = i;
            if (vkAllocateMemory(dev, &ai, nullptr, memory) == VK_SUCCESS) break;
            *memory = VK_NULL_HANDLE;
        }
    }
    if (!*memory) fatal("host-visible memory alloc");
    vkBindBufferMemory(dev, *buffer, *memory, 0);
    if (vkMapMemory(dev, *memory, 0, bytes, 0, mapped) != VK_SUCCESS)
        fatal("host-visible map");
}
} // namespace

void VulkanCommandBuffer::createInstanceBuffers() {
    VkDevice dev = device_.handle();
    constexpr VkDeviceSize inBytes = VkDeviceSize(kMaxInstances) * sizeof(InstanceData);

    // Input: host-visible (CPU uploads all instances each frame).
    cbMakeHostVisibleBuffer(device_, inBytes,
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                            &instanceInBuffer_, &instanceInMemory_, &instanceInMapped_);
    // Output: device-local (compute compacts visible instances into it).
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = inBytes;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
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
    if (!instanceOutMemory_) fatal("instance out memory");
    vkBindBufferMemory(dev, instanceOutBuffer_, instanceOutMemory_, 0);

    // Batch ranges: kMaxIndirectDraws uvec2s (x = first-instance offset).
    cbMakeHostVisibleBuffer(device_, kMaxIndirectDraws * sizeof(uint32_t) * 2,
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                            &batchRangeBuffer_, &batchRangeMemory_, &batchRangeMapped_);
}

void VulkanCommandBuffer::createCullPipeline() {
    VkDevice dev = device_.handle();
    VkShaderModule shader = cbShaderModule(
        dev, cbReadFileBytes("shaders/cull.comp.spv"));

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader;
    stage.pName = "main";

    // Set 0: 0=in SSBO, 1=out SSBO, 2=indirect cmds, 3=hiz sampler, 4=batch ranges.
    VkDescriptorSetLayoutBinding bindings[5]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[4] = {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 5;
    li.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &cullSetLayout_) != VK_SUCCESS)
        fatal("cull set layout");

    // Push constants: mat4 + 6 planes + 4 scalars = 176 bytes.
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push.offset = 0;
    push.size = 176;
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &cullSetLayout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &cullLayout_) != VK_SUCCESS)
        fatal("cull layout");

    VkComputePipelineCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pi.stage = stage;
    pi.layout = cullLayout_;
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pi, nullptr,
                                 &cullPipeline_) != VK_SUCCESS)
        fatal("cull pipeline");
    vkDestroyShaderModule(dev, shader, nullptr);

    // Descriptor sets: one per frame in flight.
    VkDescriptorPoolSize sizes[2]{};
    sizes[0] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * kFramesInFlight};
    sizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight};
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = kFramesInFlight;
    pool.poolSizeCount = 2;
    pool.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(dev, &pool, nullptr, &cullPool_) != VK_SUCCESS)
        fatal("cull pool");

    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(dev, &sci, nullptr, &sampler) != VK_SUCCESS)
        fatal("hiz sampler");

    cullSets_.resize(kFramesInFlight);
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = cullPool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &cullSetLayout_;
        if (vkAllocateDescriptorSets(dev, &ai, &cullSets_[i]) != VK_SUCCESS)
            fatal("cull set alloc");

        VkDescriptorBufferInfo in{instanceInBuffer_, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo out{instanceOutBuffer_, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo cmds{indirectBuffer_, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo ranges{batchRangeBuffer_, 0, VK_WHOLE_SIZE};
        VkDescriptorImageInfo hiz{sampler, occlusion_->hizView(),
                                  VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet writes[5]{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                     cullSets_[i], 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                     nullptr, &in, nullptr};
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                     cullSets_[i], 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                     nullptr, &out, nullptr};
        writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                     cullSets_[i], 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                     nullptr, &cmds, nullptr};
        writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                     cullSets_[i], 3, 0, 1,
                     VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &hiz, nullptr,
                     nullptr};
        writes[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                     cullSets_[i], 4, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                     nullptr, &ranges, nullptr};
        vkUpdateDescriptorSets(dev, 5, writes, 0, nullptr);
    }
    // Sampler owned by descriptor sets; destroy at pool teardown via leak-free
    // path: keep it alive by not destroying (small, one per process).
}

void VulkanCommandBuffer::createHizPipeline() {
    VkDevice dev = device_.handle();
    VkShaderModule shader = cbShaderModule(
        dev, cbReadFileBytes("shaders/hiz.comp.spv"));

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader;
    stage.pName = "main";

    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 2;
    li.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &hizSetLayout_) != VK_SUCCESS)
        fatal("hiz set layout");

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push.offset = 0;
    push.size = sizeof(int32_t) * 2;
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &hizSetLayout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &hizLayout_) != VK_SUCCESS)
        fatal("hiz layout");

    VkComputePipelineCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pi.stage = stage;
    pi.layout = hizLayout_;
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pi, nullptr,
                                 &hizPipeline_) != VK_SUCCESS)
        fatal("hiz pipeline");
    vkDestroyShaderModule(dev, shader, nullptr);

    // Sampler for depth fetch in hiz.comp.
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(dev, &sci, nullptr, &sampler) != VK_SUCCESS)
        fatal("depth sampler");

    VkDescriptorPoolSize sizes[2]{};
    sizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight};
    sizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kFramesInFlight};
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = kFramesInFlight;
    pool.poolSizeCount = 2;
    pool.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(dev, &pool, nullptr, &hizPool_) != VK_SUCCESS)
        fatal("hiz pool");

    hizSets_.resize(kFramesInFlight);
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = hizPool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &hizSetLayout_;
        if (vkAllocateDescriptorSets(dev, &ai, &hizSets_[i]) != VK_SUCCESS)
            fatal("hiz set alloc");

        VkDescriptorImageInfo depth{sampler, occlusion_->depthView(),
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo hiz{VK_NULL_HANDLE, occlusion_->hizView(),
                                  VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet writes[2]{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                     hizSets_[i], 0, 0, 1,
                     VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depth, nullptr,
                     nullptr};
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                     hizSets_[i], 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                     &hiz, nullptr, nullptr};
        vkUpdateDescriptorSets(dev, 2, writes, 0, nullptr);
    }
}

void VulkanCommandBuffer::createCameraDescriptors() {
    VkDevice dev = device_.handle();
    VkDescriptorSetLayout camL = pipeline_.cameraLayout();

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = kFramesInFlight;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kFramesInFlight;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(dev, &poolInfo, nullptr, &cameraDescriptorPool_) != VK_SUCCESS)
        fatal("camera descriptor pool");

    descriptorSets_.resize(kFramesInFlight);
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = cameraDescriptorPool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &camL;
        if (vkAllocateDescriptorSets(dev, &ai, &descriptorSets_[i]) != VK_SUCCESS)
            fatal("camera set alloc");

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
    poolSize.descriptorCount = VulkanShadowPass::kCascadeCount * kFramesInFlight;

    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = kFramesInFlight;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &shadowSamplerPool_) != VK_SUCCESS)
        fatal("shadow sampler pool");

    shadowSamplerSets_.resize(kFramesInFlight);
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = shadowSamplerPool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &layout;
        if (vkAllocateDescriptorSets(dev, &ai, &shadowSamplerSets_[i]) != VK_SUCCESS)
            fatal("shadow sampler set alloc");

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
        vkUpdateDescriptorSets(dev, static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }
}

VkDescriptorSet VulkanCommandBuffer::materialDescriptor(const Texture* tex) {
    if (!tex) tex = textures_.createFromPixels("default_white", whitePixel, 1, 1);

    auto it = materialSets_.find(tex);
    if (it != materialSets_.end()) return it->second;

    VkDevice dev = device_.handle();

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
            fatal("material pool");
    }

    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = materialPool_;
    ai.descriptorSetCount = 1;
    VkDescriptorSetLayout matL = device_.materialDescriptorLayout();
    ai.pSetLayouts = &matL;
    if (vkAllocateDescriptorSets(dev, &ai, &set) != VK_SUCCESS)
        fatal("material set alloc");

    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler = tex->sampler;
    imgInfo.imageView = tex->view;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &imgInfo;
    vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);

    materialSets_[tex] = set;
    return set;
}

// Matches shader CameraUBO: mat4 + vec4 + vec4 + vec4 + vec4 + mat4[4] + vec4.
struct FrameUBO {
    glm::mat4 viewProjection;
    glm::vec4 cameraPos;
    glm::vec4 lightDir;
    glm::vec4 lightColor;
    glm::vec4 params; // x = ambient
    glm::mat4 lightVP[VulkanShadowPass::kCascadeCount];
    glm::vec4 cascadeSplits;
};
constexpr VkDeviceSize kUboSize = sizeof(FrameUBO);

void VulkanCommandBuffer::recordFrame(uint32_t frameIndex, uint32_t imageIndex,
                                      const std::vector<PendingBatch>& batches,
                                      const std::vector<InstanceData>& instances,
                                      const glm::mat4& viewProjection,
                                      const DirectionalLight& light,
                                      const glm::vec3& cameraPos) {
    VkCommandBuffer cmd = buffers_[frameIndex];
    // --- CSM cascade fitting -------------------------------------------
    const float camNear = 0.1f;
    const float camFar = 150.0f;
    float splitsNorm[4] = {0.05f, 0.15f, 0.35f, 1.0f};
    glm::vec3 dir = glm::normalize(light.direction);

    auto ndcZ = [&](float d) {
        return ((camFar + camNear) * d - 2.0f * camFar * camNear) /
               ((camFar - camNear) * d);
    };
    glm::mat4 invVP = glm::inverse(viewProjection);

    FrameUBO ubo{};
    ubo.viewProjection = viewProjection;
    ubo.cameraPos = glm::vec4(cameraPos, 1.0f);
    ubo.lightDir = glm::vec4(dir, 0.0f);
    ubo.lightColor = glm::vec4(light.color, 1.0f);
    ubo.params = glm::vec4(0.25f, 0.0f, 0.0f, 0.0f);

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

        glm::mat4 lightProj =
            glm::ortho(minX, maxX, minY, maxY, -maxZ - 10.0f, -minZ + 10.0f);
        ubo.lightVP[c] = lightProj * lightView;
    }
    ubo.cascadeSplits = glm::vec4(splitsNorm[1], splitsNorm[2], splitsNorm[3], camFar);

    // Upload to persistently mapped UBO.
    std::memcpy(device_.mapUniform(frameIndex), &ubo, sizeof(FrameUBO));

    // --- GPU-driven instance upload --------------------------------------
    // Upload the flat instance list; batches reference it via firstInstance.
    auto* ranges = reinterpret_cast<uint32_t*>(batchRangeMapped_);
    auto* cmds = reinterpret_cast<VkDrawIndexedIndirectCommand*>(indirectMapped_);
    if (!instances.empty())
        std::memcpy(instanceInMapped_, instances.data(),
                    std::min<size_t>(instances.size(), kMaxInstances) *
                        sizeof(InstanceData));

    uint32_t totalInstances = 0;
    uint32_t drawCount = 0;
    std::vector<const PendingBatch*> accepted;
    for (const auto& batch : batches) {
        const Mesh& mesh = *batch.mesh;
        if (mesh.indices.empty() || mesh.vertices.empty() || batch.instanceCount == 0)
            continue;
        if (drawCount >= kMaxIndirectDraws) break;
        if (batch.firstInstance + batch.instanceCount > kMaxInstances) break;

        ranges[drawCount * 2 + 0] = batch.firstInstance; // batch start offset
        ranges[drawCount * 2 + 1] = batch.instanceCount;

        cmds[drawCount].indexCount = static_cast<uint32_t>(mesh.indices.size());
        cmds[drawCount].instanceCount = 0; // written by cull compute
        cmds[drawCount].firstIndex = 0;
        cmds[drawCount].vertexOffset = 0;
        cmds[drawCount].firstInstance = totalInstances; // output region = input region
        ++drawCount;

        accepted.push_back(&batch);
        totalInstances = batch.firstInstance + batch.instanceCount;
    }



    // --- Record GPU commands ---------------------------------------------
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS)
        fatal("failed to begin recording command buffer");
    // Shadow passes: one per cascade.
    for (uint32_t c = 0; c < VulkanShadowPass::kCascadeCount; ++c) {
        shadowPass_->begin(cmd, c);
        for (const auto& batch : batches)
            shadowPass_->drawBatch(cmd, *batch.mesh, batch.instanceCount,
                                   ubo.lightVP[c], instanceInBuffer_);
        shadowPass_->end(cmd);
    }

    // Depth prepass (camera POV) -> Hi-Z -> cull compute.
    if (totalInstances > 0) {
        occlusion_->beginPrepass(cmd);
        for (const PendingBatch* batch : accepted) {
            occlusion_->drawPrepass(cmd, *batch->mesh, batch->instanceCount,
                                    viewProjection, instanceInBuffer_);
        }
        occlusion_->endPrepass(cmd);

        // Hi-Z max-downsample of prepass depth.
        // Hi-Z image: UNDEFINED -> GENERAL (contents fully rewritten each frame).
        VkImageMemoryBarrier hizToGeneral{};
        hizToGeneral.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        hizToGeneral.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        hizToGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        hizToGeneral.image = occlusion_->hizImage();
        hizToGeneral.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        hizToGeneral.subresourceRange.levelCount = 1;
        hizToGeneral.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &hizToGeneral);

        // Hi-Z max-downsample of prepass depth.
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hizPipeline_);
        auto [sw, sh] = swapchain_.extent();
        int32_t srcSize[2] = {int32_t(sw), int32_t(sh)};
        vkCmdPushConstants(cmd, hizLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(srcSize), srcSize);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hizLayout_,
                                0, 1, &hizSets_[frameIndex], 0, nullptr);
        vkCmdDispatch(cmd, VulkanGpuOcclusion::kHizSize / 8,
                      VulkanGpuOcclusion::kHizSize / 8, 1);
        // hiz.comp writes -> cull.comp reads (compute-to-compute).
        VkMemoryBarrier hizBarrier{};
        hizBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        hizBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        hizBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                             &hizBarrier, 0, nullptr, 0, nullptr);

        // Frustum planes from viewProjection (Gribb-Hartmann, column-major glm).
        auto rowOf = [&](int r) {
            return glm::vec4(viewProjection[0][r], viewProjection[1][r],
                             viewProjection[2][r], viewProjection[3][r]);
        };
        glm::vec4 r0 = rowOf(0), r1 = rowOf(1), r2 = rowOf(2), r3 = rowOf(3);
        struct { float d[176 / 4]; } pushCpu{};
        float* p = pushCpu.d;
        std::memcpy(p, &viewProjection, 64); p += 16;
        glm::vec4 planes[6] = {r3 + r0, r3 - r0, r3 + r1, r3 - r1, r3 + r2, r3 - r2};
        std::memcpy(p, planes, 96); p += 24;
        uint32_t scalars[2] = {totalInstances, drawCount};
        std::memcpy(p, scalars, 8); p += 2;
        const float camNear = 0.1f, camFar = 150.0f;
        float projB = 2.0f * camNear * camFar / (camFar - camNear);
        float bias = 0.003f;
        std::memcpy(p, &projB, 4); p += 1;
        std::memcpy(p, &bias, 4);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cullPipeline_);
        vkCmdPushConstants(cmd, cullLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           176, &pushCpu);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cullLayout_,
                                0, 1, &cullSets_[frameIndex], 0, nullptr);
        vkCmdDispatch(cmd, (totalInstances + 63) / 64, 1, 1);

        // Compute SSBO writes -> indirect command reads + vertex fetch.
        VkMemoryBarrier memBarrier{};
        memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        memBarrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
                                   VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                                 VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                             0, 1, &memBarrier, 0, nullptr, 0, nullptr);
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
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.layout(),
                            0, 1, &descriptorSets_[frameIndex], 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.layout(),
                            2, 1, &shadowSamplerSets_[frameIndex], 0, nullptr);
    // IBL environment (set 3).
    VkDescriptorSet envSet = environment_->frameSet(frameIndex);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.layout(),
                            3, 1, &envSet, 0, nullptr);

    VkViewport viewport{};
    viewport.width = static_cast<float>(swapchain_.extent().width);
    viewport.height = static_cast<float>(swapchain_.extent().height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{{0, 0}, swapchain_.extent()};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Indirect draws: instance counts come from the cull compute (GPU-driven).
    uint32_t drawId = 0;
    for (const PendingBatch* batch : accepted) {
        const Mesh& mesh = *batch->mesh;

        VkBuffer vertexBuffer = device_.scratchVertexBuffer(mesh.vertices);
        VkBuffer instanceBuffer = instanceOutBuffer_;
        VkBuffer buffers[] = {vertexBuffer, instanceBuffer};
        VkDeviceSize offsets[] = {0, 0};
        vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);

        VkBuffer indexBuffer = device_.scratchIndexBuffer(mesh.indices);
        vkCmdBindIndexBuffer(cmd, indexBuffer, 0, VK_INDEX_TYPE_UINT32);

        VkDescriptorSet matSet = materialDescriptor(batch->texture);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline_.layout(), 1, 1, &matSet, 0, nullptr);

        vkCmdDrawIndexedIndirect(cmd, indirectBuffer_,
                                 drawId * sizeof(VkDrawIndexedIndirectCommand),
                                 1, sizeof(VkDrawIndexedIndirectCommand));
        ++drawId;
    }
    vkCmdEndRenderPass(cmd);

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
        fatal("failed to end recording command buffer");
}

} // namespace engine
