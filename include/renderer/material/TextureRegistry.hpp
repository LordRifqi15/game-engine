#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <cstdint>
#include <unordered_map>

namespace Engine {

class BindlessDescriptorManager;

// Slot allocation, default textures & index handles
class TextureRegistry {
public:
    static constexpr uint32_t RESERVED_SLOTS = 4; // 0-3 fallback 1x1
    static constexpr uint32_t INVALID_SLOT = UINT32_MAX;

    TextureRegistry() = default;

    void init(VkDevice device, BindlessDescriptorManager* manager);
    void shutdown();

    // Allocate slot for imageView (creates fallback if manager is null for headless)
    uint32_t allocate(VkImageView imageView);
    uint32_t allocateWithView(VkImageView imageView) { return allocate(imageView); }

    void free(uint32_t slot);

    // Ensure slots 0-3 are reserved with 1x1 fallback textures
    void ensureFallbacks();

    // Query
    bool isValid(uint32_t slot) const;
    uint32_t allocatedCount() const { return allocatedCount_; }
    uint32_t freeCount() const { return static_cast<uint32_t>(freeSlots_.size()); }

    // For tests: get fallback slot indices
    uint32_t fallbackAlbedoSlot() const { return 0; }
    uint32_t fallbackNormalSlot() const { return 1; }
    uint32_t fallbackMetallicRoughnessSlot() const { return 2; }
    uint32_t fallbackWhiteSlot() const { return 3; }

private:
    VkDevice device_{VK_NULL_HANDLE};
    BindlessDescriptorManager* manager_{nullptr};
    std::vector<uint32_t> freeSlots_;
    std::unordered_map<uint32_t, VkImageView> slotToView_;
    uint32_t nextSlot_{RESERVED_SLOTS};
    uint32_t allocatedCount_{0};
    bool hasFallbacks_{false};
};

} // namespace Engine

namespace engine {
    using TextureRegistry = ::Engine::TextureRegistry;
}
