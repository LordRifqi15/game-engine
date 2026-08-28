#include "renderer/renderer.h"

#include "platform/window.h"
#include "renderer/vulkan/vk_renderer.h"
#include "renderer/vulkan/vulkan_instance.h"

namespace engine {

// Bridges the API layer to the Vulkan backend. Vulkan types stay in this .cpp.
class Renderer::Impl {
public:
    explicit Impl(Window& window)
        : instance(window),
          backend(window, instance) {}

    VulkanInstance instance;
    VkRenderer backend;
};

Renderer::Renderer(Window& window)
    : impl_(new Impl(window)) {}

Renderer::~Renderer() {
    delete impl_;
}

class TextureCache& Renderer::textureCache() { return impl_->backend.textureCache(); }

const engine::Texture* Renderer::loadTexture(const std::string& path) {
    return impl_->backend.loadTexture(path);
}

void Renderer::beginFrame(const Camera& camera, const DirectionalLight& light) {
    impl_->backend.beginFrame(camera, light);
}

void Renderer::drawMeshInstanced(const Mesh& mesh, const InstanceData& instance,
                                 const Texture* texture) {
    impl_->backend.drawMeshInstanced(mesh, instance, texture);
}

void Renderer::updateJoints(const std::vector<glm::mat4>& joints) {
    impl_->backend.updateJoints(joints);
}

uint32_t Renderer::swapchainWidth() const { return impl_->backend.swapchainWidth(); }
uint32_t Renderer::swapchainHeight() const { return impl_->backend.swapchainHeight(); }

void Renderer::endFrame() {
    impl_->backend.drawFrame();
}

void Renderer::enableEditorOverlay(Window& window) {
    impl_->backend.enableEditorOverlay(window);
}

void Renderer::editorBeginFrame() {
    impl_->backend.editorBeginFrame();
}

void Renderer::editorEndFrame() {
    impl_->backend.editorEndFrame();
}

void Renderer::requestDepthReadback() { impl_->backend.requestDepthReadback(); }
const std::vector<float>& Renderer::depthPixels() const { return impl_->backend.depthPixels(); }
} // namespace engine
