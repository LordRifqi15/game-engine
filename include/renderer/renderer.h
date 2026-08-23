#pragma once

#include "core/camera.h"

#include <string>
#include "core/instance_data.h"
#include "core/light.h"
#include "core/material.h"
#include "core/mesh.h"
#include "core/transform.h"

// Backend selected in renderer.cpp (currently Vulkan only).
namespace engine {

class Window;
class Texture;

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

    // Depth readback for CPU occlusion (call after endFrame).
    void requestDepthReadback();
    const std::vector<float>& depthPixels() const;

    const std::vector<unsigned char>& colorPixels() const;

    // Scene-facing draw call: renders all instances of a mesh in one draw.
    void drawMeshInstanced(const Mesh& mesh, const std::vector<InstanceData>& instances,
                           const Texture* texture);

    // Submits all queued draw calls for this frame (present).
    void endFrame();

private:
    class Impl;   // backend-owned state
    Impl* impl_ = nullptr;
};

} // namespace engine
