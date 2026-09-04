#pragma once
#include "renderer/streaming/VirtualGeometryTypes.hpp"
#include "renderer/streaming/PageSlotPool.hpp"
#include <vector>
#include <mutex>
#include <cstdint>
#include <vulkan/vulkan.h>

namespace Engine {

enum class PageUpdateType {
    Evict,
    CommitResidency
};

struct PageUpdate {
    PageUpdateType type;
    uint32_t pageID;
    uint32_t slotIndex;
};

class MeshStreamingManager {
public:
    MeshStreamingManager() = default;
    ~MeshStreamingManager() = default;

    void init(VkDevice device, PageSlotPool* slotPool);
    void shutdown();

    // Called each frame with GPU readback queue (after Page_Request_Readback)
    void processReadbackRequests(const GPUPageRequestQueue& requests);

    // Apply pending residency updates to host table (called on main thread before next cull)
    void applyResidencyUpdates(VkCommandBuffer cmdBuffer, VirtualPageEntry* hostResidencyTable);

    // For tests
    size_t pendingUpdateCount() const { return stagingUpdates_.size(); }
    void clearRequests(GPUPageRequestQueue& queue);

private:
    // Simplified sync: no real thread pool for tests, process synchronously
    // In real engine, would have std::thread workers
    PageSlotPool* slotPool_{nullptr};
    VkDevice device_{VK_NULL_HANDLE};

    std::mutex uploadMutex_;
    std::vector<PageUpdate> stagingUpdates_;
    std::vector<uint8_t> stagingBuffer_; // dummy
};

} // namespace Engine

namespace engine {
    using MeshStreamingManager = ::Engine::MeshStreamingManager;
}
