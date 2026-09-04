#pragma once

#include "core/camera.h"

#include <glm/glm.hpp>

#include <string>
#include <vulkan/vulkan.h>
#include <vector>
#include "core/instance_data.h"
#include "core/light.h"
#include "core/material.h"
#include "core/mesh.h"
#include "core/transform.h"

// Backend selected in renderer.cpp (currently Vulkan only).
namespace engine {

struct RuntimeRendererInfo {
    VkInstance instance{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    VkPhysicalDevice physicalDevice{VK_NULL_HANDLE};
    VkQueue graphicsQueue{VK_NULL_HANDLE};
    VkQueue presentQueue{VK_NULL_HANDLE};
    uint32_t graphicsFamily{VK_QUEUE_FAMILY_IGNORED};
    VkSwapchainKHR swapchain{VK_NULL_HANDLE};
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainImageViews;
    VkFormat swapchainFormat{VK_FORMAT_UNDEFINED};
    VkExtent2D swapchainExtent{0, 0};
};

class Window;
class Renderer {
public:
    explicit Renderer(Window& window);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Loads a texture asset. Returns nullptr on failure.
    const class Texture* loadTexture(const std::string& path);

    // Backend texture cache (for loaders that resolve GLTF-embedded textures).
    class TextureCache& textureCache();

    // Per-frame setup: camera + light define the frame's global state.
    void beginFrame(const Camera& camera, const DirectionalLight& light);

    // Current swapchain extent (for CPU-side culling math).
    uint32_t swapchainWidth() const;
    uint32_t swapchainHeight() const;

    // Depth readback for CPU Hi-Z occlusion (call after endFrame).
    void requestDepthReadback();
    const std::vector<float>& depthPixels() const;
    const std::vector<unsigned char>& colorPixels() const;

    // Skinning: upload joint matrices for next drawFrame (set 4 SSBO).
    void updateJoints(const std::vector<glm::mat4>& joints);

    // Scene-facing draw call: renders all instances of a mesh in one draw.
    void drawMeshInstanced(const Mesh& mesh, const InstanceData& instance,
                           const Texture* texture = nullptr);

    // Submits all queued draw calls for this frame (present).
    void endFrame();

    RuntimeRendererInfo runtimeInfo() const;
    void recreateSwapchain();

    // Editor overlay (Task 031): ImGui-based node editor.
    void enableEditorOverlay(Window& window);
    void editorBeginFrame();
    void editorEndFrame();
private:
    class Impl;   // backend-owned state
    Impl* impl_ = nullptr;
};

} // namespace engine
