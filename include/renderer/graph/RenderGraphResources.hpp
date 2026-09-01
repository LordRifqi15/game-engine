#pragma once
#include <vulkan/vulkan.h>
#include <string>
#include <cstdint>

namespace Engine {

enum class ResourceType {
    Transient,
    Imported
};

enum class ResourceUsage {
    None,
    ColorAttachment,
    DepthStencilAttachment,
    ShaderRead,
    TransferSrc,
    TransferDst,
    Present
};

struct ResourceHandle {
    uint32_t id{UINT32_MAX};
    bool isValid() const { return id != UINT32_MAX; }
    bool operator==(const ResourceHandle& o) const { return id == o.id; }
    bool operator!=(const ResourceHandle& o) const { return id != o.id; }
};

struct ImageDesc {
    std::string name;
    VkFormat format{VK_FORMAT_R8G8B8A8_UNORM};
    VkExtent2D extent{0, 0};
    VkImageUsageFlags usage{0};
};

struct RenderGraphResource {
    std::string name;
    ResourceType type{ResourceType::Transient};
    ImageDesc desc;
    VkImage image{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    ResourceUsage lastUsage{ResourceUsage::None};
    // Task 044: physical aliasing
    struct PhysicalImage* physicalBinding{nullptr};
};

} // namespace Engine

namespace engine {
    using ResourceType = Engine::ResourceType;
    using ResourceUsage = Engine::ResourceUsage;
    using ResourceHandle = Engine::ResourceHandle;
    using ImageDesc = Engine::ImageDesc;
    using RenderGraphResource = Engine::RenderGraphResource;
}
