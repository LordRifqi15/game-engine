#include "renderer/scheduler/TimelineSync.hpp"
#include <unordered_map>
#include <mutex>

namespace Engine {

static std::unordered_map<VkSemaphore, uint64_t> s_hostValues;
static std::mutex s_mutex;
static uint64_t s_nextDummyId = 0x100000;

VkSemaphore TimelineSync::create(VkDevice device, uint64_t initialValue) {
    if (device == VK_NULL_HANDLE) {
        std::lock_guard<std::mutex> lock(s_mutex);
        VkSemaphore dummy = reinterpret_cast<VkSemaphore>(++s_nextDummyId);
        s_hostValues[dummy] = initialValue;
        return dummy;
    }
    VkSemaphoreTypeCreateInfo typeInfo{};
    typeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    typeInfo.initialValue = initialValue;

    VkSemaphoreCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    ci.pNext = &typeInfo;

    VkSemaphore sem = VK_NULL_HANDLE;
    if (vkCreateSemaphore(device, &ci, nullptr, &sem) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    // also track host value for monotonic checks even on real device (optional)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_hostValues[sem] = initialValue;
    }
    return sem;
}

void TimelineSync::destroy(VkDevice device, VkSemaphore sem) {
    if (sem == VK_NULL_HANDLE) return;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_hostValues.erase(sem);
    }
    if (device == VK_NULL_HANDLE) {
        // dummy, nothing to destroy
        return;
    }
    vkDestroySemaphore(device, sem, nullptr);
}

uint64_t TimelineSync::getValue(VkDevice device, VkSemaphore sem) {
    if (sem == VK_NULL_HANDLE) return 0;
    if (device == VK_NULL_HANDLE) {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto it = s_hostValues.find(sem);
        return it != s_hostValues.end() ? it->second : 0;
    }
    uint64_t v = 0;
    vkGetSemaphoreCounterValue(device, sem, &v);
    return v;
}

uint64_t TimelineSync::hostValue(VkSemaphore sem) {
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_hostValues.find(sem);
    return it != s_hostValues.end() ? it->second : 0;
}

void TimelineSync::hostSignal(VkSemaphore sem, uint64_t value) {
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_hostValues.find(sem);
    if (it != s_hostValues.end()) {
        // enforce monotonicity (spec: must strictly increase)
        if (value > it->second) it->second = value;
    } else {
        s_hostValues[sem] = value;
    }
}

} // namespace Engine
