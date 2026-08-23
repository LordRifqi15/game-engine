#include "renderer/vulkan/vk_command_buffer.h"

#include "renderer/vulkan/vk_device.h"
#include "renderer/vulkan/vk_pipeline.h"
#include "renderer/vulkan/vk_swapchain.h"
#include "renderer/vulkan/texture_cache.h"


#include <array>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace engine {

namespace {

void fatal(const char* msg) {
    std::fprintf(stderr, "Fatal: %s\n", msg);
    std::exit(EXIT_FAILURE);
}

} // namespace

namespace {

constexpr uint32_t kFramesInFlight = 2;

// 1x1 white fallback texture data.
const unsigned char whitePixel[4] = {255, 255, 255, 255};

// Matches shader CameraUBO: mat4 + vec4 + vec4 (std140-style alignment).
struct FrameUBO {
    glm::mat4 viewProjection;
    glm::vec4 cameraPos;  // xyz, w unused
    glm::vec4 lightDir;   // normalized direction light travels
    glm::vec4 lightColor; // rgb
    glm::vec4 params;     // x = ambient strength (reserved)
};
constexpr VkDeviceSize kCameraUboSize = sizeof(FrameUBO);

} // namespace

VulkanCommandBuffer::VulkanCommandBuffer(VulkanDevice& device, VulkanSwapchain& swapchain,
                                         const VulkanPipeline& pipeline, TextureCache& textures)
    : device_(device), swapchain_(swapchain), pipeline_(pipeline), textures_(textures) {
    device_.createFrameFences(kFramesInFlight);
    device_.createUniformBuffers(kFramesInFlight, kCameraUboSize);
    buffers_.resize(kFramesInFlight);
    for (auto& cmd : buffers_) {
        cmd = device_.allocateCommandBuffer();
    }
    createCameraDescriptors();
}

void VulkanCommandBuffer::createCameraDescriptors() {
    VkDevice dev = device_.handle();
    VkDescriptorSetLayout cameraLayout = device_.cameraDescriptorLayout();

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = kFramesInFlight;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kFramesInFlight;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;

    if (vkCreateDescriptorPool(dev, &poolInfo, nullptr, &cameraDescriptorPool_) != VK_SUCCESS) {
        fatal("failed to create camera descriptor pool");
    }

    descriptorSets_.resize(kFramesInFlight);
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = cameraDescriptorPool_;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &cameraLayout;

        if (vkAllocateDescriptorSets(dev, &allocInfo, &descriptorSets_[i]) != VK_SUCCESS) {
            fatal("failed to allocate descriptor set");
        }

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

VulkanCommandBuffer::~VulkanCommandBuffer() {
    VkDevice dev = device_.handle();
    if (cameraDescriptorPool_ != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(dev, cameraDescriptorPool_, nullptr);
    if (materialPool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(dev, materialPool_, nullptr);
}

VkDescriptorSet VulkanCommandBuffer::materialDescriptor(const Texture* tex) {
    auto it = materialSets_.find(tex);
    if (it != materialSets_.end()) return it->second;

    // Grow the pool on demand: fresh pool with room for one more set.
    VkDevice dev = device_.handle();

    static uint32_t poolSets = 0;
    if (materialPool_ == VK_NULL_HANDLE || poolSets % 8 == 0) {
        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = 8;
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 8;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        if (vkCreateDescriptorPool(dev, &poolInfo, nullptr, &materialPool_) != VK_SUCCESS)
            fatal("failed to create material descriptor pool");
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = materialPool_;
    allocInfo.descriptorSetCount = 1;
    VkDescriptorSetLayout matLayout = device_.materialDescriptorLayout();
    allocInfo.pSetLayouts = &matLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
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

    // Per-frame camera + light -> UBO (persistently mapped, coherent).
    FrameUBO ubo{};
    ubo.viewProjection = viewProjection;
    ubo.cameraPos = glm::vec4(cameraPos, 1.0f);
    ubo.lightDir = glm::vec4(glm::normalize(light.direction), 0.0f);
    ubo.lightColor = glm::vec4(light.color, 1.0f);
    ubo.params = glm::vec4(0.25f, 0.0f, 0.0f, 0.0f); // ambient

    // Upload to the persistently mapped UBO for this frame-in-flight.
    std::memcpy(device_.mapUniform(frameIndex), &ubo, sizeof(FrameUBO));

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
        fatal("failed to begin recording command buffer");
    }

    std::array<VkClearValue, 2> clearValues{};
    clearValues[1].depthStencil = {1.0f, 0};
    clearValues[0].color = {{0.02f, 0.02f, 0.05f, 1.0f}}; // near-black blue


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

    VkViewport viewport{};
    viewport.width = static_cast<float>(swapchain_.extent().width);
    viewport.height = static_cast<float>(swapchain_.extent().height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{{0, 0}, swapchain_.extent()};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // One instanced draw per (mesh, texture) group. Instance buffer uploaded
    // per draw (ponytail: persistent instance buffer pool when churn justifies it).
    for (const auto& batch : batches) {
        const Mesh& mesh = *batch.mesh;
        const auto& instances = *batch.instances;
        if (instances.empty() || mesh.indices.empty() || mesh.vertices.empty()) continue;

        VkDeviceSize vertexOffset = 0;
        VkBuffer vertexBuffer = device_.scratchVertexBuffer(mesh.vertices);
        VkBuffer instanceBuffer = device_.scratchVertexBuffer(instances);
        VkBuffer buffers[] = {vertexBuffer, instanceBuffer};
        VkDeviceSize offsets[] = {0, 0};
        vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);

        VkBuffer indexBuffer = device_.scratchIndexBuffer(mesh.indices);
        vkCmdBindIndexBuffer(cmd, indexBuffer, 0, VK_INDEX_TYPE_UINT32);

        // Material texture (set 1). Null texture -> 1x1 white fallback.
        VkDescriptorSet matSet = materialDescriptor(
            batch.texture ? batch.texture
                          : textures_.createFromPixels("default_white", whitePixel, 1, 1));
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline_.layout(), 1, 1, &matSet, 0, nullptr);

        vkCmdDrawIndexed(cmd, static_cast<uint32_t>(mesh.indices.size()),
                         static_cast<uint32_t>(instances.size()), 0, 0, 0);
    }

    vkCmdEndRenderPass(cmd);

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        fatal("failed to end recording command buffer");
    }
}




} // namespace engine
