#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "core/instance_data.h"
#include "core/mesh.h"

#include <array>
#include <vector>

// Cascaded shadow maps: kCascadeCount depth-only passes (one per cascade),
// each rendering instanced meshes from the light's point of view with its
// own lightVP push constant.
namespace engine {

class VulkanDevice;
class VulkanSwapchain;

class VulkanShadowPass {
public:
    static constexpr uint32_t kCascadeCount = 4;
    static constexpr uint32_t kSize = 1024;

    VulkanShadowPass(VulkanDevice& device, const VulkanSwapchain& swapchain);
    ~VulkanShadowPass();

    VulkanShadowPass(const VulkanShadowPass&) = delete;
    VulkanShadowPass& operator=(const VulkanShadowPass&) = delete;

    // Begins the shadow render pass for `cascade` (clears to 1.0, binds pipeline).
    void begin(VkCommandBuffer cmd, uint32_t cascade);
    // Draws one batch (mesh + instances) — same instance buffers as main pass.
    void drawBatch(VkCommandBuffer cmd, const Mesh& mesh,
                   uint32_t instanceCount, const glm::mat4& lightVP,
                   VkBuffer instanceBuffer);
    // Modern path: caller-owned vertex/index buffers with instance range.
    void drawBatchAt(VkCommandBuffer cmd, VkBuffer vertexBuffer, VkBuffer indexBuffer,
                     uint32_t indexCount, uint32_t instanceCount, uint32_t firstInstance,
                     const glm::mat4& lightVP, VkBuffer instanceBuffer);
    // Ends the render pass.
    void end(VkCommandBuffer cmd);

    // For main-pass descriptor writes (set 2, bindings 0..3).
    VkImageView view(uint32_t cascade) const { return views_[cascade]; }
    VkSampler sampler() const { return sampler_; }
private:
    void createResources();
    void createRenderPass();
    void createPipeline();

    VulkanDevice& device_;
    const VulkanSwapchain& swapchain_;

    std::array<VkImage, kCascadeCount> images_{};
    std::array<VkDeviceMemory, kCascadeCount> memories_{};
    std::array<VkImageView, kCascadeCount> views_{};
    std::array<VkFramebuffer, kCascadeCount> framebuffers_{};

    VkSampler sampler_ = VK_NULL_HANDLE;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;

    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

} // namespace engine
