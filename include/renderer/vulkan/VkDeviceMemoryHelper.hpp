#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>

namespace Engine {

uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties);
VkDeviceMemory allocateImageMemory(VkDevice device, VkPhysicalDevice physicalDevice, VkImage image, VkMemoryPropertyFlags properties);
void createImageWithMemory(VkDevice device, VkPhysicalDevice physicalDevice,
                           VkFormat format, VkExtent2D extent, VkImageUsageFlags usage,
                           VkImage* outImage, VkDeviceMemory* outMemory, VkImageView* outView);
void createImageWithMemory(VkDevice device, VkPhysicalDevice physicalDevice,
                           VkFormat format, VkExtent2D extent, uint32_t mipLevels, VkImageUsageFlags usage,
                           VkImage* outImage, VkDeviceMemory* outMemory, VkImageView* outView);

} // namespace Engine
