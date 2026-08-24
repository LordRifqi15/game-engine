#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

// Backend facade: owns device, swapchain, pipeline, command buffers, and the
// per-frame submit loop.

#include "core/camera.h"
#include "core/instance_data.h"
#include "core/light.h"
#include "core/mesh.h"

#include "renderer/api/render_command_buffer.h"
#include "renderer/vulkan/texture_cache.h"
#include "renderer/vulkan/vk_swapchain.h"

#include <vector>

namespace engine {

class Window;
class VulkanInstance;
class VulkanDevice;
class VulkanPipeline;
class VulkanCommandBuffer;

class VkRenderer {
public:
    explicit VkRenderer(Window& window, VulkanInstance& vk);
    ~VkRenderer();

    VkRenderer(const VkRenderer&) = delete;
    VkRenderer& operator=(const VkRenderer&) = delete;

    // Per-frame setup: stores camera + light for this frame.
    void beginFrame(const Camera& camera, const DirectionalLight& light);

    // Scene-facing draw call: renders all instances of a mesh in one draw.
    void drawMeshInstanced(const Mesh& mesh, const InstanceData& instance,
                           const Texture* texture);

    // Draws one frame (submits all queued draws).
    void drawFrame();

    uint32_t swapchainWidth() const { return swapchain_->width(); }
    uint32_t swapchainHeight() const { return swapchain_->height(); }

    const Texture* loadTexture(const std::string& path) { return textureCache_->load(path); }
    TextureCache& textureCache() { return *textureCache_; }

    // Depth readback for CPU Hi-Z occlusion (call after drawFrame).
    void requestDepthReadback() { swapchain_->requestDepthReadback(); }
    const std::vector<float>& depthPixels() const { return swapchain_->depthPixels(); }

private:
    Window& window_;
    VulkanInstance& vk_;

    VulkanDevice* device_ = nullptr;
    VulkanSwapchain* swapchain_ = nullptr;
    VulkanPipeline* pipeline_ = nullptr;
    TextureCache* textureCache_ = nullptr;
    VulkanCommandBuffer* commandBuffers_ = nullptr;

    // Batches queued by drawMeshInstanced(), consumed at drawFrame().
    std::vector<PendingBatch> pendingBatches_;
    std::vector<InstanceData> allInstances_;
    uint32_t nextInstanceOffset_ = 0;

    glm::mat4 viewProjection_{1.0f};
    DirectionalLight light_{};
    glm::vec3 cameraPos_{0.0f, 0.0f, 3.0f};
    uint32_t currentFrame_ = 0;
    static constexpr uint32_t kMaxFramesInFlight = 2;
};

} // namespace engine
