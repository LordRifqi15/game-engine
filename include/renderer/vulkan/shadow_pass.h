#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "core/instance_data.h"
#include "core/mesh.h"

#include <vector>

// Single-cascade directional shadow pass: depth-only image + render pass +
// pipeline. Renders instanced meshes from the light's point of view.
namespace engine {

class VulkanDevice;
class VulkanSwapchain;

class VulkanShadowPass {
public:
    VulkanShadowPass(VulkanDevice& device, const VulkanSwapchain& swapchain,
                     VkDescriptorSetLayout cameraSetLayout);
    ~VulkanShadowPass();

    VulkanShadowPass(const VulkanShadowPass&) = delete;
    VulkanShadowPass& operator=(const VulkanShadowPass&) = delete;

    // Begins the shadow render pass (clears to 1.0, binds pipeline).
    void begin(VkCommandBuffer cmd);
    // Draws one batch (mesh + instances) — same instance buffers as main pass.
    void drawBatch(VkCommandBuffer cmd, const Mesh& mesh,
                   const std::vector<InstanceData>& instances,
                   const glm::mat4& lightVP);
    // Ends the render pass.
    void end(VkCommandBuffer cmd);

    // For main-pass descriptor writes.
    VkImageView view() const { return view_; }
    VkSampler sampler() const { return sampler_; }
    VkDescriptorSetLayout layout() const { return setLayout_; }

private:
    void createResources();
    void createRenderPass();
    void createPipelineAndDescriptors();

    VulkanDevice& device_;
    const VulkanSwapchain& swapchain_;

    static constexpr uint32_t kSize = 2048;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;   // set 2: shadow sampler
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptorSets_;        // per frame-in-flight

    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

} // namespace engine
