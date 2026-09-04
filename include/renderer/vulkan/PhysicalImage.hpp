#pragma once
#include <vulkan/vulkan.h>
#include <string>

namespace Engine {

struct PhysicalImage {
    VkImage image{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkExtent2D extent{0, 0};
    VkImageUsageFlags usage{0};
    uint32_t mipLevels{1};
    VkImageLayout currentLayout{VK_IMAGE_LAYOUT_UNDEFINED};
    uint32_t lastUsedPassIndex{UINT32_MAX};
    uint64_t lastUsedFrameIndex{0};
};

} // namespace Engine

namespace engine {
    using PhysicalImage = ::Engine::PhysicalImage;
}
