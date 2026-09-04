#pragma once
#include "renderer/vulkan/PhysicalImage.hpp"
#include "renderer/graph/RenderGraphResources.hpp"
#include <vulkan/vulkan.h>
#include <vector>
#include <memory>

namespace Engine {

class TransientResourcePool {
public:
    static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;

    TransientResourcePool(VkDevice device, VkPhysicalDevice physicalDevice);
    TransientResourcePool(const TransientResourcePool&) = delete;
    TransientResourcePool& operator=(const TransientResourcePool&) = delete;
    ~TransientResourcePool();

    // Acquire compatible physical image or allocate a new one
    PhysicalImage* acquireImage(const ImageDesc& desc, uint32_t passIndex, uint64_t frameIndex);
    // overload for lifetime-aware acquire
    PhysicalImage* acquireImage(const ImageDesc& desc, uint32_t firstPass, uint32_t lastPass, uint64_t frameIndex);

    // Advance frame index and reset pass markers
    void advanceFrame(uint64_t currentFrameIndex);

    // Destroy all pooled resources (on shutdown or swapchain resize)
    void clear();

    size_t getTotalAllocationCount() const;
    size_t getFrameAllocationCount(uint32_t frameSlot) const;

private:
    PhysicalImage* createPhysicalImage(const ImageDesc& desc);
    bool isCompatible(const PhysicalImage& physical, const ImageDesc& desc) const;

    VkDevice device_{VK_NULL_HANDLE};
    VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};

    // Per-frame pools to avoid CPU/GPU write hazards
    std::vector<std::unique_ptr<PhysicalImage>> framePools_[MAX_FRAMES_IN_FLIGHT];
    uint32_t currentFrameSlot_{0};
};

} // namespace Engine

namespace engine {
    using TransientResourcePool = ::Engine::TransientResourcePool;
}
