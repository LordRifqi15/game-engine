#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "core/mesh.h"

#include <glm/glm.hpp>

#include <cstdint>

namespace engine {

class VulkanDevice;
class VulkanSwapchain;

// GPU Hi-Z occlusion culling support:
//   1. Depth prepass: depth-only render pass from the camera POV.
//   2. Hi-Z: compute-shader max-downsample of the prepass depth into a
//      kHizSize^2 R32_SFLOAT storage image (conservative: stores farthest).
//   3. cull.comp samples the Hi-Z to skip occluded instances (no CPU readback).
class VulkanGpuOcclusion {
public:
    static constexpr uint32_t kHizSize = 512;

    VulkanGpuOcclusion(VulkanDevice& device, const VulkanSwapchain& swapchain);
    ~VulkanGpuOcclusion();

    VulkanGpuOcclusion(const VulkanGpuOcclusion&) = delete;
    VulkanGpuOcclusion& operator=(const VulkanGpuOcclusion&) = delete;

    // Depth-only prepass (camera POV). viewProjection via push constant.
    // instanceBuffer supplies binding 1 (instance model matrices).
    void beginPrepass(VkCommandBuffer cmd);
    void drawPrepass(VkCommandBuffer cmd, const Mesh& mesh, uint32_t instanceCount,
                     const glm::mat4& viewProjection, VkBuffer instanceBuffer);
    void endPrepass(VkCommandBuffer cmd);

    // Views for hiz.comp descriptor writes (depth sampled, hiz storage).
    VkImageView depthView() const { return depthView_; }
    VkImageView hizView() const { return hizView_; }
    VkImage hizImage() const { return hizImage_; }

private:
    void createDepthResources();
    void createHizResources();
    void createRenderPassAndFramebuffer();
    void createPrepassPipeline();

    VulkanDevice& device_;
    const VulkanSwapchain& swapchain_;

    uint32_t width_ = 0, height_ = 0;

    // Depth prepass (camera POV).
    VkImage depthImage_ = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory_ = VK_NULL_HANDLE;
    VkImageView depthView_ = VK_NULL_HANDLE;
    VkFramebuffer depthFramebuffer_ = VK_NULL_HANDLE;
    VkRenderPass depthRenderPass_ = VK_NULL_HANDLE;
    VkPipelineLayout prepassLayout_ = VK_NULL_HANDLE;
    VkPipeline prepassPipeline_ = VK_NULL_HANDLE;

    // Downsampled depth (written by hiz.comp, read by cull.comp).
    VkImage hizImage_ = VK_NULL_HANDLE;
    VkDeviceMemory hizMemory_ = VK_NULL_HANDLE;
    VkImageView hizView_ = VK_NULL_HANDLE;
};

} // namespace engine
