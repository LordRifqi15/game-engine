#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>

namespace Engine {

// Vulkan Timeline Semaphore helpers. Headless (VK_NULL_HANDLE) returns dummy handles and
// host-side monotonic counters so unit tests can validate without a real device.
class TimelineSync {
public:
    // Create a timeline semaphore with initial value. Dummy when device is null.
    static VkSemaphore create(VkDevice device, uint64_t initialValue = 0);

    static void destroy(VkDevice device, VkSemaphore sem);

    // Host query of current timeline value (vkGetSemaphoreCounterValue). Returns 0 for dummy.
    static uint64_t getValue(VkDevice device, VkSemaphore sem);

    // Monotonicity helper for tests: next signal value must be > current.
    static bool isMonotonic(uint64_t prev, uint64_t next) { return next > prev; }

    // For headless tests: host-side counter map (sem handle -> value)
    static uint64_t hostValue(VkSemaphore sem);
    static void hostSignal(VkSemaphore sem, uint64_t value);
};

} // namespace Engine

namespace engine {
    using TimelineSync = ::Engine::TimelineSync;
}
