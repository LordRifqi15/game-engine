#include "renderer/streaming/PageSlotPool.hpp"
#include <algorithm>

namespace Engine {

void PageSlotPool::init(VkDevice device, VkPhysicalDevice physicalDevice) {
    device_ = device;
    physicalDevice_ = physicalDevice;
    lruList_.clear();
    pageToSlotMap_.clear();
    freeSlots_.clear();
    freeSlots_.reserve(TOTAL_PHYSICAL_SLOTS);
    for (uint32_t i = 0; i < TOTAL_PHYSICAL_SLOTS; ++i) freeSlots_.push_back(TOTAL_PHYSICAL_SLOTS - 1 - i); // reverse so pop gives 0 first

    if (device_ == VK_NULL_HANDLE) {
        physicalBuffer_ = reinterpret_cast<VkBuffer>(0x5000);
        physicalMemory_ = reinterpret_cast<VkDeviceMemory>(0x5001);
        return;
    }
    VkBufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size = uint64_t(PAGE_SIZE_BYTES) * TOTAL_PHYSICAL_SLOTS;
    ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(device_, &ci, nullptr, &physicalBuffer_);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, physicalBuffer_, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = 0; // simplified
    vkAllocateMemory(device_, &ai, nullptr, &physicalMemory_);
    vkBindBufferMemory(device_, physicalBuffer_, physicalMemory_, 0);
}

void PageSlotPool::shutdown() {
    lruList_.clear();
    pageToSlotMap_.clear();
    freeSlots_.clear();
    if (device_ != VK_NULL_HANDLE) {
        if (physicalBuffer_ != VK_NULL_HANDLE) vkDestroyBuffer(device_, physicalBuffer_, nullptr);
        if (physicalMemory_ != VK_NULL_HANDLE) vkFreeMemory(device_, physicalMemory_, nullptr);
    }
    physicalBuffer_ = VK_NULL_HANDLE;
    physicalMemory_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    physicalDevice_ = VK_NULL_HANDLE;
}

uint32_t PageSlotPool::allocateSlot(uint32_t virtualPageID, uint32_t& outEvictedPageID) {
    outEvictedPageID = UINT32_MAX;
    // If already resident, touch and return existing slot
    auto it = pageToSlotMap_.find(virtualPageID);
    if (it != pageToSlotMap_.end()) {
        // Move to front (most recent)
        lruList_.erase(it->second.second);
        lruList_.push_front(virtualPageID);
        it->second.second = lruList_.begin();
        return it->second.first;
    }

    uint32_t slot = INVALID_SLOT;
    if (!freeSlots_.empty()) {
        slot = freeSlots_.back();
        freeSlots_.pop_back();
    } else {
        // Evict LRU (back)
        if (lruList_.empty()) return INVALID_SLOT;
        uint32_t lruPage = lruList_.back();
        lruList_.pop_back();
        auto evictIt = pageToSlotMap_.find(lruPage);
        if (evictIt != pageToSlotMap_.end()) {
            slot = evictIt->second.first;
            outEvictedPageID = lruPage;
            pageToSlotMap_.erase(evictIt);
        }
    }

    if (slot == INVALID_SLOT) return INVALID_SLOT;

    lruList_.push_front(virtualPageID);
    pageToSlotMap_[virtualPageID] = {slot, lruList_.begin()};
    return slot;
}

void PageSlotPool::touchSlot(uint32_t virtualPageID) {
    auto it = pageToSlotMap_.find(virtualPageID);
    if (it == pageToSlotMap_.end()) return;
    lruList_.erase(it->second.second);
    lruList_.push_front(virtualPageID);
    it->second.second = lruList_.begin();
}

void PageSlotPool::freeSlot(uint32_t virtualPageID) {
    auto it = pageToSlotMap_.find(virtualPageID);
    if (it == pageToSlotMap_.end()) return;
    uint32_t slot = it->second.first;
    lruList_.erase(it->second.second);
    pageToSlotMap_.erase(it);
    freeSlots_.push_back(slot);
}

bool PageSlotPool::isResident(uint32_t virtualPageID) const {
    return pageToSlotMap_.find(virtualPageID) != pageToSlotMap_.end();
}

} // namespace Engine
