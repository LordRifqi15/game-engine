#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "renderer/api/render_swapchain.h"

#include <vector>

// Owns swapchain, image views, render pass, framebuffers, and semaphores.
namespace engine {

class Window;
class VulkanDevice;
class VulkanInstance;

class VulkanSwapchain final : public RenderSwapchain {
public:
    VulkanSwapchain(Window& window, VulkanInstance& vk, VulkanDevice& device);
    ~VulkanSwapchain() override;

    VulkanSwapchain(const VulkanSwapchain&) = delete;
    VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;

    // RenderSwapchain
    bool acquireNextImage(uint32_t frameIndex) override { return acquireOrRecreate(frameIndex, currentImage_); }
    void present(uint32_t imageIndex, uint32_t frameIndex) override;
    uint32_t imageCount() const override { return static_cast<uint32_t>(images_.size()); }
    uint32_t width() const override { return extent_.width; }
    uint32_t height() const override { return extent_.height; }

    // Backend accessors for sibling classes (same module, allowed).
    VkFormat format() const { return format_; }
    VkExtent2D extent() const { return extent_; }
    VkSwapchainKHR handle() const { return swapchain_; }
    const std::vector<VkImage>& images() const { return images_; }
    const std::vector<VkImageView>& views() const { return views_; }
    VkRenderPass renderPass() const { return renderPass_; }
    VkFramebuffer framebuffer(uint32_t index) const { return framebuffers_[index]; }
    void requestDepthReadback();
    const std::vector<float>& depthPixels() const { return depthPixels_; }

    // DEBUG: color readback for automated visual verification.

    // DEBUG: color readback for automated visual verification.
    VkImageView depthView() const { return depthView_; }
    VkSemaphore acquireSemaphoreForFrame(uint32_t frameIndex) const { return imageAvailable_[frameIndex]; }
    VkSemaphore renderFinishedSemaphore(uint32_t imageIndex) const { return renderFinished_[imageIndex]; }

    // Returns false when the caller should skip this frame (recreated inside).
    bool acquireOrRecreate(uint32_t frameIndex, uint32_t& outImage);

    void recreate();

private:
    void createDepthResources();
    void destroyDepthResources();

    // Copies depthImage_ into a host-visible buffer (CPU Hi-Z source).
    // Must be called outside a render pass; image must be in transfer src layout.


    struct Support {
        VkSurfaceCapabilitiesKHR caps{};
        std::vector<VkSurfaceFormatKHR> formats;
        std::vector<VkPresentModeKHR> presentModes;
    };

    void create();
    void createViews();
    void createRenderPass();
    void createFramebuffers();
    void createPerImageSemaphores();
    void destroy(bool includeSwapchain);

    Support querySupport(VkPhysicalDevice device) const;

    Window& window_;
    VulkanInstance& vk_;
    VulkanDevice& device_;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<VkFramebuffer> framebuffers_;
    std::vector<VkSemaphore> renderFinished_; // per swapchain image
    std::vector<VkSemaphore> imageAvailable_; // per frame in flight

    VkImage depthImage_ = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory_ = VK_NULL_HANDLE;
    VkImageView depthView_ = VK_NULL_HANDLE;
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

    // CPU readback: staging buffer + command pool dedicated to copy.
    VkBuffer readbackBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory readbackMemory_ = VK_NULL_HANDLE;
    void* readbackMapped_ = nullptr;
    VkCommandPool readbackPool_ = VK_NULL_HANDLE;
    std::vector<float> depthPixels_;

    // DEBUG: in-frame color readback (copy inside command buffer, pre-present).
    void ensureColorReadbackResources();
public:
private:
    friend class VulkanSwapchain; // debug color readback uses internals

    uint32_t currentImage_ = 0;
    uint32_t lastAcquiredIndex_ = 0;
};

} // namespace engine
