#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "renderer/api/render_pipeline.h"

// Owns pipeline layout and the triangle graphics pipeline.
// Render pass comes from VulkanSwapchain (framebuffer format owner).
namespace engine {

class VulkanDevice;
class VulkanSwapchain;

class VulkanPipeline final : public RenderPipeline {
public:
    // cameraSetLayout: descriptor set layout bound at set 0 (camera UBO).
    VulkanPipeline(VulkanDevice& device, const VulkanSwapchain& swapchain,
                   VkDescriptorSetLayout cameraSetLayout,
                   VkDescriptorSetLayout materialSetLayout,
                   VkDescriptorSetLayout shadowSamplerSetLayout,
                   VkDescriptorSetLayout envSetLayout,
                   VkDescriptorSetLayout skinningSetLayout);
    ~VulkanPipeline() override;

    VulkanPipeline(const VulkanPipeline&) = delete;
    VulkanPipeline& operator=(const VulkanPipeline&) = delete;

    // Backend accessors for command recording (same module, allowed).
    VkPipeline handle() const { return pipeline_; }
    VkPipelineLayout layout() const { return layout_; }
    VkDescriptorSetLayout cameraLayout() const { return cameraSetLayout_; }

private:
    void create();

    VulkanDevice& device_;
    const VulkanSwapchain& swapchain_;

    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout cameraSetLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

} // namespace engine
