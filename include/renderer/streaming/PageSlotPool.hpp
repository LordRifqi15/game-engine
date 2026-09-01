#pragma once
#include "renderer/streaming/VirtualGeometryTypes.hpp"
#include <vector>
#include <list>
#include <unordered_map>

namespace Engine {

class PageSlotPool {
public:
    static constexpr uint32_t PAGE_SIZE_BYTES = 64 * 1024; // 64 KB fixed page slot
    static constexpr uint32_t TOTAL_PHYSICAL_SLOTS = 2048; // 128 MB VRAM Pool
    static constexpr uint32_t INVALID_SLOT = UINT32_MAX;

    PageSlotPool() = default;
    ~PageSlotPool() { if (device_ != VK_NULL_HANDLE) shutdown(); }

    void init(VkDevice device, VkPhysicalDevice physicalDevice);
    void shutdown();

    // Allocates a physical slot, evicting the least recently used page if full
    uint32_t allocateSlot(uint32_t virtualPageID, uint32_t& outEvictedPageID);
    void touchSlot(uint32_t virtualPageID);
    void freeSlot(uint32_t virtualPageID);

    VkBuffer getPhysicalStorageBuffer() const { return physicalBuffer_; }
    size_t allocatedCount() const { return pageToSlotMap_.size(); }
    size_t freeSlotCount() const { return freeSlots_.size(); }
    bool isResident(uint32_t virtualPageID) const;

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
    VkBuffer physicalBuffer_{VK_NULL_HANDLE};
    VkDeviceMemory physicalMemory_{VK_NULL_HANDLE};

    std::list<uint32_t> lruList_; // Front: Most recent, Back: Least recent
    std::unordered_map<uint32_t, std::pair<uint32_t, std::list<uint32_t>::iterator>> pageToSlotMap_;
    std::vector<uint32_t> freeSlots_;
};

} // namespace Engine

namespace engine {
    using PageSlotPool = Engine::PageSlotPool;
}
