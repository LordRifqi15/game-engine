#include "renderer/vulkan/VkDeviceMemoryHelper.hpp"
#include <stdexcept>

namespace Engine {

uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    if (physicalDevice == VK_NULL_HANDLE) return 0;
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties) return i;
    }
    throw std::runtime_error("findMemoryType: no suitable type");
}

VkDeviceMemory allocateImageMemory(VkDevice device, VkPhysicalDevice physicalDevice, VkImage image, VkMemoryPropertyFlags properties) {
    if (device == VK_NULL_HANDLE || image == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, image, &req);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = req.size;
    allocInfo.memoryTypeIndex = findMemoryType(physicalDevice, req.memoryTypeBits, properties);
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (vkAllocateMemory(device, &allocInfo, nullptr, &mem) != VK_SUCCESS) throw std::runtime_error("allocateImageMemory failed");
    vkBindImageMemory(device, image, mem, 0);
    return mem;
}

void createImageWithMemory(VkDevice device, VkPhysicalDevice physicalDevice,
                           VkFormat format, VkExtent2D extent, VkImageUsageFlags usage,
                           VkImage* outImage, VkDeviceMemory* outMemory, VkImageView* outView) {
    if (device == VK_NULL_HANDLE) {
        // dummy handles for tests / headless
        static uint64_t nextId = 0x1000;
        *outImage = reinterpret_cast<VkImage>(++nextId);
        *outMemory = reinterpret_cast<VkDeviceMemory>(++nextId);
        *outView = reinterpret_cast<VkImageView>(++nextId);
        return;
    }
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {extent.width, extent.height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &ci, nullptr, outImage) != VK_SUCCESS) throw std::runtime_error("vkCreateImage failed");
    *outMemory = allocateImageMemory(device, physicalDevice, *outImage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = *outImage;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    if (format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_X8_D24_UNORM_PACK32) aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.baseMipLevel = 0;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.baseArrayLayer = 0;
    vi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &vi, nullptr, outView) != VK_SUCCESS) throw std::runtime_error("vkCreateImageView failed");
}

} // namespace Engine
