#include "renderer/vulkan/vk_command_buffer.h"

#include "renderer/vulkan/texture_cache.h"
#include "renderer/vulkan/vk_device.h"
#include "renderer/vulkan/vk_pipeline.h"

#include <glm/gtc/matrix_transform.hpp>
#include <unistd.h>
#include "renderer/vulkan/vk_pipeline.h"
#include "renderer/vulkan/vk_swapchain.h"

#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

    device_.createFrameFences(kFramesInFlight);
    device_.createUniformBuffers(kFramesInFlight, kCameraUboSize);
    buffers_.resize(kFramesInFlight);
    for (auto& cmd : buffers_) cmd = device_.allocateCommandBuffer();

    createCameraDescriptors();
    createShadowSamplerSets();
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
    VkCommandBuffer cmd = buffers_[frameIndex];

    // --- CSM cascade fitting -------------------------------------------
    const float camNear = 0.1f;
    const float camFar = 150.0f; // shadow draw distance
    float splitsNorm[4] = {0.05f, 0.15f, 0.35f, 1.0f};

    auto ndcZ = [&](float d) {
        return ((camFar + camNear) * d - 2.0f * camFar * camNear) /
               ((camFar - camNear) * d);
    };

    glm::vec3 dir = glm::normalize(light.direction);

    // Per-frame UBO payload.
    FrameUBO ubo{};
    ubo.viewProjection = viewProjection;
    ubo.cameraPos = glm::vec4(cameraPos, 1.0f);
    ubo.lightDir = glm::vec4(dir, 0.0f);
    ubo.lightColor = glm::vec4(light.color, 1.0f);
    ubo.params = glm::vec4(0.25f, 0.0f, 0.0f, 0.0f); // ambient

    glm::mat4 invVP = glm::inverse(viewProjection);

    // Shadow pass first: render all batches per cascade.
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
            minX = std::min(minX, ls.x);
            maxX = std::max(maxX, ls.x);
            minY = std::min(minY, ls.y);
            maxY = std::max(maxY, ls.y);
            minZ = std::min(minZ, ls.z);
            maxZ = std::max(maxZ, ls.z);
        }
        // Texel snapping: round extents so the ortho box moves in texel steps.
        float texelX = (maxX - minX) / static_cast<float>(VulkanShadowPass::kSize);
        if (texelX > 0) {
            minX = std::floor(minX / texelX) * texelX;
            maxX = std::ceil(maxX / texelX) * texelX;
        }
        float texelY = (maxY - minY) / static_cast<float>(VulkanShadowPass::kSize);
        if (texelY > 0) {
            minY = std::floor(minY / texelY) * texelY;
            maxY = std::ceil(maxY / texelY) * texelY;
        }

        glm::mat4 lightProj = glm::ortho(minX, maxX, minY, maxY, -maxZ - 10.0f,
                                         -minZ + 10.0f);
        ubo.lightVP[c] = lightProj * lightView;

    }

    ubo.cascadeSplits =
        glm::vec4(splitsNorm[1], splitsNorm[2], splitsNorm[3], camFar);

    // Upload to the persistently mapped UBO for this frame-in-flight.
    std::memcpy(device_.mapUniform(frameIndex), &ubo, sizeof(FrameUBO));

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS)
        fatal("failed to begin recording command buffer");

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

    // Shadow passes: one per cascade, recorded before the main pass.
    for (uint32_t c = 0; c < VulkanShadowPass::kCascadeCount; ++c) {
        shadowPass_->begin(cmd, c);
        for (const auto& batch : batches)
            shadowPass_->drawBatch(cmd, *batch.mesh, *batch.instances, ubo.lightVP[c]);
        shadowPass_->end(cmd);
    }

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

    for (const auto& batch : batches) {
        const Mesh& mesh = *batch.mesh;
        const auto& instances = *batch.instances;
        if (instances.empty() || mesh.indices.empty() || mesh.vertices.empty()) continue;

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

        vkCmdDrawIndexed(cmd, static_cast<uint32_t>(mesh.indices.size()),
                         static_cast<uint32_t>(instances.size()), 0, 0, 0);
    }

    vkCmdEndRenderPass(cmd);

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
        fatal("failed to end recording command buffer");
}


} // namespace engine
