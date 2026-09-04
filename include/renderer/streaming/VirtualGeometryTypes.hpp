#pragma once
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include <cstdint>

namespace Engine {

// Page state flags
enum PageStatusFlags : uint32_t {
    PAGE_STATUS_UNLOADED = 0,
    PAGE_STATUS_REQUESTED = 1,
    PAGE_STATUS_RESIDENT = 2
};

// Virtual page descriptor in GPU ResidencyTable SSBO
struct alignas(16) VirtualPageEntry {
    uint32_t physicalSlotIndex; // Slot index in PhysicalPageBuffer (if resident)
    uint32_t status;            // PAGE_STATUS_*
    uint32_t meshletCount;      // Number of meshlets inside this page
    uint32_t parentPageID;      // Fallback LOD parent page ID (UINT32_MAX if root)
};

// Queue populated by GPU compute culling
struct alignas(16) GPUPageRequestQueue {
    uint32_t count;
    uint32_t maxRequests;
    uint32_t padding[2];
    uint32_t requestedPageIDs[4096];
};

struct alignas(16) PageStreamingUniforms {
    uint32_t maxVirtualPages;
    uint32_t maxPhysicalSlots;
    float lodErrorThreshold;
    uint32_t padding;
};

static_assert(sizeof(VirtualPageEntry) == 16, "VirtualPageEntry 16");
static_assert(sizeof(GPUPageRequestQueue) == 16 + 4096*4, "GPUPageRequestQueue size");
static_assert(sizeof(PageStreamingUniforms) == 16, "PageStreamingUniforms 16");

} // namespace Engine

namespace engine {
    using VirtualPageEntry = ::Engine::VirtualPageEntry;
    using GPUPageRequestQueue = ::Engine::GPUPageRequestQueue;
    using PageStreamingUniforms = ::Engine::PageStreamingUniforms;
}
