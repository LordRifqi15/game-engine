#include "renderer/vulkan/vk_swapchain.h"

#include "platform/window.h"
#include "renderer/vulkan/vulkan_instance.h"
#include "renderer/vulkan/vk_device.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <array>
#include <cstring>
#include <vector>

namespace engine {

namespace {

void fatal(const char* msg) {
    std::fprintf(stderr, "Fatal: %s\n", msg);
    std::exit(EXIT_FAILURE);
}

} // namespace

VulkanSwapchain::VulkanSwapchain(Window& window, VulkanInstance& vk, VulkanDevice& device)
    : window_(window), vk_(vk), device_(device) {
    create();
    createViews();
    createDepthResources();
    createRenderPass();
    createFramebuffers();

    // Acquire-side semaphores follow frame count.
    imageAvailable_.resize(2); // matches VkRenderer::kMaxFramesInFlight
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (auto& sem : imageAvailable_) {
        if (vkCreateSemaphore(device_.handle(), &semInfo, nullptr, &sem) != VK_SUCCESS) {
            fatal("failed to create acquire semaphore");
        }
    }
}

VulkanSwapchain::~VulkanSwapchain() {
    destroy(/*includeSwapchain=*/true);
}

void VulkanSwapchain::create() {
    Support support = querySupport(device_.physical());

    VkSurfaceFormatKHR format = support.formats[0];
    for (const auto& f : support.formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            format = f;
            break;
        }
    }

    // Mailbox when available, else FIFO (always supported).
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    for (auto m : support.presentModes) {
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) {
            presentMode = m;
            break;
        }
    }

    VkExtent2D extent = support.caps.currentExtent;
    if (extent.width == std::numeric_limits<uint32_t>::max()) {
        // Wayland/some compositors report unset extent: use framebuffer size.
        int w = 0, h = 0;
        glfwGetFramebufferSize(window_.handle(), &w, &h);
        extent.width = std::clamp<uint32_t>(static_cast<uint32_t>(w),
                                            support.caps.minImageExtent.width,
                                            support.caps.maxImageExtent.width);
        extent.height = std::clamp<uint32_t>(static_cast<uint32_t>(h),
                                             support.caps.minImageExtent.height,
                                             support.caps.maxImageExtent.height);
    }

    uint32_t imageCount = support.caps.minImageCount + 1;
    if (support.caps.maxImageCount > 0 && imageCount > support.caps.maxImageCount) {
        imageCount = support.caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = vk_.surface();
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = format.format;
    createInfo.imageColorSpace = format.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    createInfo.preTransform = support.caps.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;

    auto q = device_.findQueueFamilies(device_.physical());
    uint32_t families[] = {static_cast<uint32_t>(q.graphics), static_cast<uint32_t>(q.present)};
    if (q.graphics != q.present) {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = families;
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    if (vkCreateSwapchainKHR(device_.handle(), &createInfo, nullptr, &swapchain_) != VK_SUCCESS) {
        fatal("failed to create swapchain");
    }

    vkGetSwapchainImagesKHR(device_.handle(), swapchain_, &imageCount, nullptr);
    images_.resize(imageCount);
    vkGetSwapchainImagesKHR(device_.handle(), swapchain_, &imageCount, images_.data());

    format_ = format.format;
    extent_ = extent;

    createPerImageSemaphores();
}

void VulkanSwapchain::createViews() {
    views_.resize(images_.size());
    for (size_t i = 0; i < images_.size(); ++i) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = images_[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format_;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device_.handle(), &viewInfo, nullptr, &views_[i]) != VK_SUCCESS) {
            fatal("failed to create image view");
        }
    }
}

void VulkanSwapchain::createRenderPass() {
    VkAttachmentDescription depth{};
    depth.format = kDepthFormat;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    // STORE (not DONT_CARE): CPU Hi-Z reads this back after the frame.
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription color{};
    color.format = format_;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 1;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;
    // DEBUG ISOLATION: no depth

    VkSubpassDependency dep{};
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    std::array<VkAttachmentDescription, 2> attachments = {color, depth};
    VkRenderPassCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    createInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    createInfo.pAttachments = attachments.data();
    createInfo.subpassCount = 1;
    createInfo.pSubpasses = &subpass;
    createInfo.dependencyCount = 1;
    createInfo.pDependencies = &dep;

    if (vkCreateRenderPass(device_.handle(), &createInfo, nullptr, &renderPass_) != VK_SUCCESS) {
        fatal("failed to create render pass");
    }
}

void VulkanSwapchain::createFramebuffers() {
    framebuffers_.resize(views_.size());
    for (size_t i = 0; i < views_.size(); ++i) {
        std::array<VkImageView, 2> fbAttachments = {views_[i], depthView_};
        VkFramebufferCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        createInfo.renderPass = renderPass_;
        createInfo.attachmentCount = static_cast<uint32_t>(fbAttachments.size());
        createInfo.pAttachments = fbAttachments.data();
        createInfo.width = extent_.width;
        createInfo.height = extent_.height;
        createInfo.layers = 1;

        if (vkCreateFramebuffer(device_.handle(), &createInfo, nullptr, &framebuffers_[i]) != VK_SUCCESS) {
            fatal("failed to create framebuffer");
        }
    }
}

void VulkanSwapchain::createPerImageSemaphores() {
    renderFinished_.resize(images_.size());
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (auto& sem : renderFinished_) {
        if (vkCreateSemaphore(device_.handle(), &semInfo, nullptr, &sem) != VK_SUCCESS) {
            fatal("failed to create per-image semaphore");
        }
    }
}

bool VulkanSwapchain::acquireOrRecreate(uint32_t frameIndex, uint32_t& outImage) {
    VkResult acquire = vkAcquireNextImageKHR(
        device_.handle(), swapchain_, UINT64_MAX,
        imageAvailable_[frameIndex], VK_NULL_HANDLE, &outImage);

    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate();
        return false;
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
        fatal("failed to acquire swapchain image");
    }
    lastAcquiredIndex_ = outImage;
    return true;
}

void VulkanSwapchain::present(uint32_t imageIndex, uint32_t /*frameIndex*/) {
    VkSemaphore signalSemaphores[] = {renderFinished_[imageIndex]};

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain_;
    presentInfo.pImageIndices = &imageIndex;

    VkResult result = vkQueuePresentKHR(device_.presentQueue(), &presentInfo);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        recreate();
    } else if (result != VK_SUCCESS) {
        fatal("failed to present swapchain image");
    }
}

void VulkanSwapchain::recreate() {
    int w = 0, h = 0;
    glfwGetFramebufferSize(window_.handle(), &w, &h);
    while (w == 0 || h == 0) {
        glfwWaitEvents();
        glfwGetFramebufferSize(window_.handle(), &w, &h);
    }

    device_.waitIdle();
    destroyDepthResources();
    destroy(/*includeSwapchain=*/true);
    create();
    createViews();
    createDepthResources();
    createFramebuffers();
}

void VulkanSwapchain::destroy(bool includeSwapchain) {
    if (readbackBuffer_ != VK_NULL_HANDLE) {
        if (readbackMapped_) vkUnmapMemory(device_.handle(), readbackMemory_);
        vkDestroyBuffer(device_.handle(), readbackBuffer_, nullptr);
        vkFreeMemory(device_.handle(), readbackMemory_, nullptr);
        vkDestroyCommandPool(device_.handle(), readbackPool_, nullptr);
        readbackBuffer_ = VK_NULL_HANDLE;
        readbackMemory_ = VK_NULL_HANDLE;
        readbackMapped_ = nullptr;
        readbackPool_ = VK_NULL_HANDLE;
    }
    destroyDepthResources();
    for (auto sem : renderFinished_) vkDestroySemaphore(device_.handle(), sem, nullptr);
    renderFinished_.clear();
    for (auto fb : framebuffers_) vkDestroyFramebuffer(device_.handle(), fb, nullptr);
    framebuffers_.clear();
    for (auto view : views_) vkDestroyImageView(device_.handle(), view, nullptr);
    views_.clear();
    if (renderPass_ != VK_NULL_HANDLE) vkDestroyRenderPass(device_.handle(), renderPass_, nullptr);
    renderPass_ = VK_NULL_HANDLE;
    if (includeSwapchain && swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_.handle(), swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

void VulkanSwapchain::createDepthResources() {
    VkDevice dev = device_.handle();

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = kDepthFormat;
    imageInfo.extent = {extent_.width, extent_.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT; // CPU Hi-Z readback
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(dev, &imageInfo, nullptr, &depthImage_) != VK_SUCCESS) {
        fatal("failed to create depth image");
    }

    VkMemoryRequirements reqs{};
    vkGetImageMemoryRequirements(dev, depthImage_, &reqs);

    // Device-local memory: reuse the finder pattern from the device class.
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(device_.physical(), &memProps);
    uint32_t memoryType = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((reqs.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ==
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            memoryType = i;
            break;
        }
    }
    if (memoryType == UINT32_MAX) fatal("no device-local memory type for depth");

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = reqs.size;
    allocInfo.memoryTypeIndex = memoryType;

    if (vkAllocateMemory(dev, &allocInfo, nullptr, &depthMemory_) != VK_SUCCESS) {
        fatal("failed to allocate depth memory");
    }
    if (vkBindImageMemory(dev, depthImage_, depthMemory_, 0) != VK_SUCCESS) {
        fatal("failed to bind depth image memory");
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = depthImage_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kDepthFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(dev, &viewInfo, nullptr, &depthView_) != VK_SUCCESS) {
        fatal("failed to create depth image view");
    }

    // Layout transitions UNDEFINED -> DEPTH_ATTACHMENT_OPTIMAL happen inside
    // the render pass via loadOp=CLEAR on first use.
}


void VulkanSwapchain::destroyDepthResources() {
    if (depthView_ != VK_NULL_HANDLE) vkDestroyImageView(device_.handle(), depthView_, nullptr);
    if (depthImage_ != VK_NULL_HANDLE) vkDestroyImage(device_.handle(), depthImage_, nullptr);
    if (depthMemory_ != VK_NULL_HANDLE) vkFreeMemory(device_.handle(), depthMemory_, nullptr);
    depthView_ = VK_NULL_HANDLE;
    depthImage_ = VK_NULL_HANDLE;
    depthMemory_ = VK_NULL_HANDLE;
}

void VulkanSwapchain::requestDepthReadback() {
    VkDevice dev = device_.handle();

    // Lazily create staging buffer + pool at current extent.
    VkDeviceSize bytes = static_cast<VkDeviceSize>(extent_.width) * extent_.height * sizeof(float);
    if (readbackBuffer_ == VK_NULL_HANDLE) {
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = bytes;
        bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(dev, &bufInfo, nullptr, &readbackBuffer_) != VK_SUCCESS)
            fatal("failed to create readback buffer");

        VkMemoryRequirements reqs{};
        vkGetBufferMemoryRequirements(dev, readbackBuffer_, &reqs);

        VkPhysicalDeviceMemoryProperties memProps{};
        vkGetPhysicalDeviceMemoryProperties(device_.physical(), &memProps);
        uint32_t memType = UINT32_MAX;
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            if ((reqs.memoryTypeBits & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags &
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                memType = i;
                break;
            }
        }
        if (memType == UINT32_MAX) fatal("no host-visible memory for readback");

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = reqs.size;
        allocInfo.memoryTypeIndex = memType;
        if (vkAllocateMemory(dev, &allocInfo, nullptr, &readbackMemory_) != VK_SUCCESS)
            fatal("failed to allocate readback memory");
        if (vkBindBufferMemory(dev, readbackBuffer_, readbackMemory_, 0) != VK_SUCCESS)
            fatal("failed to bind readback buffer");

        if (vkMapMemory(dev, readbackMemory_, 0, bytes, 0, &readbackMapped_) != VK_SUCCESS)
            fatal("failed to map readback memory");

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = device_.graphicsFamily();
        if (vkCreateCommandPool(dev, &poolInfo, nullptr, &readbackPool_) != VK_SUCCESS)
            fatal("failed to create readback command pool");
    }

    // One-shot copy: depth image -> buffer.
    // Depth image is in DEPTH_STENCIL_ATTACHMENT_OPTIMAL after the render pass;
    // transition -> TRANSFER_SRC, copy, transition back. Queue wait keeps it simple
    // (ponytail: proper sync objects when frame overlap matters).
    VkCommandBufferAllocateInfo cbAlloc{};
    cbAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbAlloc.commandPool = readbackPool_;
    cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAlloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(dev, &cbAlloc, &cmd);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    VkImageMemoryBarrier toSrc{};
    toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.image = depthImage_;
    toSrc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    toSrc.subresourceRange.levelCount = 1;
    toSrc.subresourceRange.layerCount = 1;
    toSrc.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &toSrc);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {extent_.width, extent_.height, 1};
    vkCmdCopyImageToBuffer(cmd, depthImage_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readbackBuffer_, 1, &region);

    VkImageMemoryBarrier toAttach = toSrc;
    toAttach.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toAttach.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    toAttach.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toAttach.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &toAttach);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    vkQueueSubmit(device_.graphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(device_.graphicsQueue());
    vkFreeCommandBuffers(dev, readbackPool_, 1, &cmd);

    // D32_SFLOAT reads back as native floats — direct copy out.
    depthPixels_.resize(static_cast<size_t>(extent_.width) * extent_.height);
    std::memcpy(depthPixels_.data(), readbackMapped_, depthPixels_.size() * sizeof(float));

}

VulkanSwapchain::Support VulkanSwapchain::querySupport(VkPhysicalDevice physical) const {
    Support s;
    VkInstance instance = vk_.handle();
    VkSurfaceKHR surface = vk_.surface();
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &s.caps);

    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr);
    s.formats.resize(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, s.formats.data());

    vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &count, nullptr);
    s.presentModes.resize(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &count, s.presentModes.data());
    return s;
}

} // namespace engine
