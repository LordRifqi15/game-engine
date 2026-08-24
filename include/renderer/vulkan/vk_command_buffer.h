#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "core/instance_data.h"
#include "core/light.h"
#include "core/mesh.h"

#include "renderer/api/render_command_buffer.h"
#include "renderer/vulkan/environment.h"
#include "renderer/vulkan/shadow_pass.h"

#include <array>
#include <unordered_map>
#include <vector>

// Owns the per-frame command buffers, camera descriptor sets, and records queued draws.
namespace engine {

class VulkanDevice;
class VulkanSwapchain;
class VulkanPipeline;
class TextureCache;
class Texture;

class VulkanCommandBuffer final : public RenderCommandBuffer {
public:
    VulkanCommandBuffer(VulkanDevice& device, VulkanSwapchain& swapchain,
                        const VulkanPipeline& pipeline, TextureCache& textures);
    ~VulkanCommandBuffer() override;

    // RenderCommandBuffer: viewProjection written to the frame's camera UBO.
    void recordFrame(uint32_t frameIndex, uint32_t imageIndex,
                     const std::vector<PendingBatch>& batches,
                     const glm::mat4& viewProjection,
                     const DirectionalLight& light,
                     const glm::vec3& cameraPos) override;

    // Backend accessors (same module, allowed).
    VkCommandBuffer handle(uint32_t frameIndex) const { return buffers_[frameIndex]; }



private:
    void createCameraDescriptors();
    void createShadowSamplerSets();
    // Allocates (and caches) a set 1 descriptor for the texture.
    VkDescriptorSet materialDescriptor(const Texture* tex);

    VulkanDevice& device_;
    VulkanSwapchain& swapchain_;
    const VulkanPipeline& pipeline_;
    TextureCache& textures_;

    std::vector<VkCommandBuffer> buffers_; // one per frame in flight

    // Camera UBO descriptor pool + sets (layout owned by VulkanDevice).
    VkDescriptorPool cameraDescriptorPool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptorSets_; // one per frame in flight

    // Shadow map sampler sets (set 2), layout owned by device.
    std::vector<VkDescriptorSet> shadowSamplerSets_;
    VkDescriptorPool shadowSamplerPool_ = VK_NULL_HANDLE;
    VulkanShadowPass* shadowPass_ = nullptr;
    VulkanEnvironment* environment_ = nullptr;

    // Material texture descriptors (set 1): one set per unique texture.
    VkDescriptorPool materialPool_ = VK_NULL_HANDLE;
    std::unordered_map<const Texture*, VkDescriptorSet> materialSets_;
};

} // namespace engine
