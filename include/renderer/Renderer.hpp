#pragma once
#include "renderer/FrameContext.hpp"
#include "renderer/graph/RenderGraph.hpp"
#include "renderer/graph/TransientResourcePool.hpp"
#include "renderer/scheduler/FrameScheduler.hpp"
#include <vulkan/vulkan.h>
// Scene registry type comes from the engine ECS; headless tests compile with
// -I include so this resolves without pulling the full engine.
#if __has_include(<entt/entt.hpp>)
#include <entt/entt.hpp>
#else
#include "core/registry.h"
#endif

struct GLFWwindow;
struct ImGuiContext;

namespace Engine {

struct GPUScene;
class SceneRenderer;

enum class RendererBackendMode {
    Legacy,
    RenderGraph
};

struct RuntimeSwapchainState {
    VkSwapchainKHR handle{VK_NULL_HANDLE};
    VkQueue presentQueue{VK_NULL_HANDLE};
    std::vector<VkImage> images;
    std::vector<VkImageView> imageViews;
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkExtent2D extent{0, 0};
};

struct RuntimeRendererConfig {
    VkInstance instance{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    VkPhysicalDevice physicalDevice{VK_NULL_HANDLE};
    VkQueue graphicsQueue{VK_NULL_HANDLE};
    VkQueue computeQueue{VK_NULL_HANDLE};
    VkQueue transferQueue{VK_NULL_HANDLE};
    QueueFamilyIndices queueIndices{};
    RuntimeSwapchainState swapchain;
};


class Renderer {
public:
    static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;

    Renderer() = default;
    ~Renderer() { if (device_ != VK_NULL_HANDLE) shutdown(); }

    void init(VkDevice device, VkPhysicalDevice physicalDevice, const QueueFamilyIndices& queueIndices);
    void init(const RuntimeRendererConfig& config);
    void updateSwapchain(const RuntimeSwapchainState& swapchain);
    void shutdown();

    bool beginFrame(FrameContext& outContext, float dt);
    bool beginFrame(FrameContext& outContext, float dt, entt::registry& registry);

    // 2. Render Frame: assembles DAG, compiles barriers, schedules multi-queue submission
    void renderFrame(FrameContext& ctx);
    void renderFrame(FrameContext& ctx, const GPUScene* scene);

    // Authoritative scene path (Task 053). When set, buildFrameGraph delegates
    // to the scene renderer and records carry its pipelines.
    void setSceneRenderer(class SceneRenderer* renderer) { sceneRenderer_ = renderer; }

    // 3. End Frame: presents and advances frame slot
    void endFrame(const FrameContext& ctx);

    // Editor overlay on its own ImGui context (dynamic rendering, no legacy
    // VkRenderPass). Must be called with the platform window before use.
    void initEditorOverlay(GLFWwindow* window, VkFormat colorFormat);
    void editorBegin();
    void editorEnd();
    bool editorReady() const { return uiReady_; }

    void onResize(uint32_t newWidth, uint32_t newHeight);

    // For tests and staging
    uint64_t currentFrameIndex() const { return currentFrameIndex_; }
    bool framebufferResized() const { return framebufferResized_; }
    FrameScheduler& scheduler() { return scheduler_; }
    const RuntimeSwapchainState& swapchain() const { return swapchain_; }

private:
    void buildFrameGraph(RenderGraph& graph, const FrameContext& ctx);

    // Pass callbacks stay graph-owned; individual GPU passes are added as they
    // acquire live pipelines and resource bindings.
    void recordShadowPass(VkCommandBuffer cb, const FrameContext& ctx);
    void recordClusterCullCompute(VkCommandBuffer cb, const FrameContext& ctx);
    void recordHiZBuild(VkCommandBuffer cb, const FrameContext& ctx);
    void recordMeshletCull(VkCommandBuffer cb, const FrameContext& ctx);
    void recordGBufferDraw(VkCommandBuffer cb, const FrameContext& ctx);
    void recordDeferredLighting(VkCommandBuffer cb, const FrameContext& ctx);
    void recordSkyboxAndTransparents(VkCommandBuffer cb, const FrameContext& ctx);
    void recordTonemapping(VkCommandBuffer cb, const FrameContext& ctx);
    void recordEditorUI(VkCommandBuffer cb, const FrameContext& ctx);
    void shutdownEditorOverlay();

    VkInstance instance_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};
    VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
    VkQueue graphicsQueue_{VK_NULL_HANDLE};
    uint32_t graphicsFamily_{UINT32_MAX};
    TransientResourcePool transientPool_{VK_NULL_HANDLE, VK_NULL_HANDLE};
    FrameScheduler scheduler_{VK_NULL_HANDLE, QueueFamilyIndices{}};
    RuntimeSwapchainState swapchain_{};
    ImGuiContext* uiContext_{nullptr};
    VkDescriptorPool uiPool_{VK_NULL_HANDLE};
    bool uiReady_{false};

    VkFence inFlightFences_[MAX_FRAMES_IN_FLIGHT]{};
    VkSemaphore imageAvailable_[MAX_FRAMES_IN_FLIGHT]{};
    // Present semaphore per swapchain image: an image is never re-acquired
    // while its present is pending, so per-image signals cannot collide.
    std::vector<VkSemaphore> renderFinishedByImage_;
    uint64_t currentFrameIndex_{0};
    bool framebufferResized_{false};
    VkExtent2D renderExtent_{1920, 1080};

    // Persistent resources for HiZ / previous frame
    ResourceHandle previousFrameDepth_{};
    SceneRenderer* sceneRenderer_{nullptr};
    const GPUScene* activeScene_{nullptr};
};

} // namespace Engine
namespace renderer {
    using Renderer = ::Engine::Renderer;
    using RendererBackendMode = ::Engine::RendererBackendMode;
    using RuntimeRendererConfig = ::Engine::RuntimeRendererConfig;
    using RuntimeSwapchainState = ::Engine::RuntimeSwapchainState;
}

