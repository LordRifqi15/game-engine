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
    ComputeRead,
    ComputeWrite,
    TransferSrc,
    TransferDst,
    Present
};

enum class BufferUsage {
    None,
    ComputeRead,
    ComputeWrite,
    FragmentRead,
    VertexRead,
    TransferSrc,
    TransferDst,
    IndirectBuffer
};

struct BufferHandle {
    uint32_t id{UINT32_MAX};
    bool isValid() const { return id != UINT32_MAX; }
    bool operator==(const BufferHandle& o) const { return id == o.id; }
    bool operator!=(const BufferHandle& o) const { return id != o.id; }
};

struct BufferDesc {
    std::string name;
    size_t size{0};
    VkBufferUsageFlags usage{0};
};

struct RenderGraphBufferResource {
    std::string name;
    ResourceType type{ResourceType::Transient};
    BufferDesc desc;
    VkBuffer buffer{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    BufferUsage lastUsage{BufferUsage::None};
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
    uint32_t mipLevels{1};
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
    using BufferUsage = Engine::BufferUsage;
    using ResourceHandle = Engine::ResourceHandle;
    using BufferHandle = Engine::BufferHandle;
    using ImageDesc = Engine::ImageDesc;
    using BufferDesc = Engine::BufferDesc;
    using RenderGraphResource = Engine::RenderGraphResource;
    using RenderGraphBufferResource = Engine::RenderGraphBufferResource;
}
