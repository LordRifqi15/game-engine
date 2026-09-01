#include "renderer/streaming/MeshStreamingManager.hpp"
#include <algorithm>
#include <cstring>

namespace Engine {

void MeshStreamingManager::init(VkDevice device, PageSlotPool* slotPool) {
    device_ = device;
    slotPool_ = slotPool;
    stagingUpdates_.clear();
    stagingBuffer_.clear();
}

void MeshStreamingManager::shutdown() {
    stagingUpdates_.clear();
    stagingBuffer_.clear();
    slotPool_ = nullptr;
    device_ = VK_NULL_HANDLE;
}

void MeshStreamingManager::processReadbackRequests(const GPUPageRequestQueue& requests) {
    uint32_t numRequests = std::min(requests.count, requests.maxRequests);
    if (numRequests == 0) return;
    if (!slotPool_) return;

    for (uint32_t i = 0; i < numRequests; ++i) {
        uint32_t pageID = requests.requestedPageIDs[i];
        // Deduplicate: if page already has pending update, skip
        bool alreadyPending = false;
        {
            std::lock_guard<std::mutex> lock(uploadMutex_);
            for (auto &u : stagingUpdates_) if (u.pageID == pageID && u.type == PageUpdateType::CommitResidency) { alreadyPending = true; break; }
        }
        if (alreadyPending) continue;

        // Simulate async load: allocate slot (may evict)
        uint32_t evictedPageID = UINT32_MAX;
        uint32_t slotIndex = slotPool_->allocateSlot(pageID, evictedPageID);

        std::lock_guard<std::mutex> lock(uploadMutex_);
        if (evictedPageID != UINT32_MAX) {
            stagingUpdates_.push_back({PageUpdateType::Evict, evictedPageID, 0});
        }
        // Simulate staging upload (dummy data)
        // In real engine: std::vector<uint8_t> pageData = diskLoader_.loadPageData(pageID);
        // stagingUploader_.stageUpload(slotIndex * PageSlotPool::PAGE_SIZE_BYTES, pageData.data(), pageData.size());
        (void)slotIndex;
        stagingUpdates_.push_back({PageUpdateType::CommitResidency, pageID, slotIndex});
    }
}

void MeshStreamingManager::applyResidencyUpdates(VkCommandBuffer cmdBuffer, VirtualPageEntry* hostResidencyTable) {
    (void)cmdBuffer;
    std::lock_guard<std::mutex> lock(uploadMutex_);
    for (const auto& update : stagingUpdates_) {
        if (update.type == PageUpdateType::CommitResidency) {
            hostResidencyTable[update.pageID].physicalSlotIndex = update.slotIndex;
            hostResidencyTable[update.pageID].status = PAGE_STATUS_RESIDENT;
        } else if (update.type == PageUpdateType::Evict) {
            hostResidencyTable[update.pageID].status = PAGE_STATUS_UNLOADED;
            hostResidencyTable[update.pageID].physicalSlotIndex = UINT32_MAX;
        }
    }
    stagingUpdates_.clear();
}

void MeshStreamingManager::clearRequests(GPUPageRequestQueue& queue) {
    queue.count = 0;
}

} // namespace Engine
