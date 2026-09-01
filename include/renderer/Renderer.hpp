#pragma once
#include "renderer/FrameContext.hpp"
#include "renderer/graph/RenderGraph.hpp"
#include "renderer/graph/TransientResourcePool.hpp"
#include "renderer/scheduler/FrameScheduler.hpp"
#include <vulkan/vulkan.h>
// entt is optional for headless tests; provide stub if not available
#if __has_include(<entt/entt.hpp>)
#include <entt/entt.hpp>
#else
namespace entt { struct registry {}; }
#endif

namespace Engine {

class Renderer {
public:
    static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;

    Renderer() = default;
    ~Renderer() { if (device_ != VK_NULL_HANDLE) shutdown(); }

    void init(VkDevice device, VkPhysicalDevice physicalDevice, const QueueFamilyIndices& queueIndices);
    void shutdown();

    // 1. Begin Frame: Waits for CPU fence, acquires swapchain image, prepares context
    bool beginFrame(FrameContext& outContext, float dt, entt::registry& registry);

    // 2. Render Frame: Assembles DAG, compiles barriers, schedules multi-queue submission
    void renderFrame(FrameContext& ctx);

    // 3. End Frame: Submits present command, advances timeline semaphores, flips frame slot
    void endFrame(const FrameContext& ctx);

    void onResize(uint32_t newWidth, uint32_t newHeight);

    // For tests
    uint64_t currentFrameIndex() const { return currentFrameIndex_; }
    bool framebufferResized() const { return framebufferResized_; }
    FrameScheduler& scheduler() { return scheduler_; }

private:
    void buildFrameGraph(RenderGraph& graph, const FrameContext& ctx);

    // Helpers for recording (stubs for headless)
    void recordShadowPass(VkCommandBuffer cb, const FrameContext& ctx);
    void recordClusterCullCompute(VkCommandBuffer cb, const FrameContext& ctx);
    void recordHiZBuild(VkCommandBuffer cb, const FrameContext& ctx);
    void recordMeshletCull(VkCommandBuffer cb, const FrameContext& ctx);
    void recordGBufferDraw(VkCommandBuffer cb, const FrameContext& ctx);
    void recordDeferredLighting(VkCommandBuffer cb, const FrameContext& ctx);
    void recordSkyboxAndTransparents(VkCommandBuffer cb, const FrameContext& ctx);
    void recordTonemapping(VkCommandBuffer cb, const FrameContext& ctx);
    void recordEditorUI(VkCommandBuffer cb, const FrameContext& ctx);

    VkDevice device_{VK_NULL_HANDLE};
    VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
    TransientResourcePool transientPool_{VK_NULL_HANDLE, VK_NULL_HANDLE};
    FrameScheduler scheduler_{VK_NULL_HANDLE, QueueFamilyIndices{}};

    VkFence inFlightFences_[MAX_FRAMES_IN_FLIGHT]{};
    uint64_t currentFrameIndex_{0};
    bool framebufferResized_{false};
    VkExtent2D renderExtent_{1920, 1080};

    // Persistent resources for HiZ / previous frame
    ResourceHandle previousFrameDepth_{};
};

} // namespace Engine

namespace engine {
    using Renderer = Engine::Renderer;
}
