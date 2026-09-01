#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace Engine {

enum class QueueType : uint8_t {
    Graphics = 0,
    AsyncCompute = 1,
    Transfer = 2,
    Count = 3
};

struct QueueFamilyIndices {
    uint32_t graphicsFamily{UINT32_MAX};
    uint32_t computeFamily{UINT32_MAX};
    uint32_t transferFamily{UINT32_MAX};

    bool hasDedicatedCompute() const {
        return computeFamily != UINT32_MAX && computeFamily != graphicsFamily;
    }
    bool hasDedicatedTransfer() const {
        return transferFamily != UINT32_MAX && transferFamily != graphicsFamily && transferFamily != computeFamily;
    }
};

struct QueueContext {
    VkQueue queue{VK_NULL_HANDLE};
    uint32_t familyIndex{UINT32_MAX};
    VkSemaphore timelineSemaphore{VK_NULL_HANDLE};
    uint64_t currentTimelineValue{0};
};

struct CrossQueueSync {
    QueueType srcQueue;
    QueueType dstQueue;
    uint64_t waitValue;
    uint64_t signalValue;
};

} // namespace Engine

namespace engine {
    using QueueType = Engine::QueueType;
    using QueueFamilyIndices = Engine::QueueFamilyIndices;
    using QueueContext = Engine::QueueContext;
    using CrossQueueSync = Engine::CrossQueueSync;
}
