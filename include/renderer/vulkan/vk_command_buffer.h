#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "core/instance_data.h"
#include "core/light.h"
#include "core/mesh.h"

#include <functional>
#include <glm/glm.hpp>

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
class VulkanGpuOcclusion;
class VulkanCommandBuffer final : public RenderCommandBuffer {
public:
    VulkanCommandBuffer(VulkanDevice& device, VulkanSwapchain& swapchain,
                        const VulkanPipeline& pipeline, TextureCache& textures);
    ~VulkanCommandBuffer() override;

    // RenderCommandBuffer: viewProjection written to the frame's camera UBO.
    void recordFrame(uint32_t frameIndex, uint32_t imageIndex,
                     const std::vector<PendingBatch>& batches,
                     const std::vector<InstanceData>& instances,
                     const glm::mat4& viewProjection,
                     const DirectionalLight& light,
                     const glm::vec3& cameraPos) override;
    static constexpr uint32_t kMaxInstances = 65536;
    static constexpr uint32_t kMaxJoints = 128;

    // Backend accessors (same module, allowed).
    VkCommandBuffer handle(uint32_t frameIndex) const { return buffers_[frameIndex]; }
    void updateJoints(uint32_t frameIndex, const std::vector<glm::mat4>& mats);

    // Editor overlay (Task 031): invoked inside the main render pass, before End.
    void setOverlay(std::function<void(VkCommandBuffer)> cb) { overlay_ = std::move(cb); }

 private:
    void createCameraDescriptors();
    void createShadowSamplerSets();
    void createIndirectBuffer();
    void createInstanceBuffers();
    void createJointBuffer();
    void createCullPipeline();
    void createHizPipeline();
    // Allocates (and caches) a set 1 descriptor for the texture.
    VkDescriptorSet materialDescriptor(const Texture* tex);
    // Allocates (and caches) a set 1 descriptor for the texture.

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
    VulkanGpuOcclusion* occlusion_ = nullptr;

    // Material texture descriptors (set 1): one set per unique texture.
    VkDescriptorPool materialPool_ = VK_NULL_HANDLE;
    std::unordered_map<const Texture*, VkDescriptorSet> materialSets_;

    // Indirect draw buffer: host-visible, filled from CPU each frame.
    static constexpr uint32_t kMaxIndirectDraws = 16;
    VkBuffer indirectBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory indirectMemory_ = VK_NULL_HANDLE;
    void* indirectMapped_ = nullptr;
    uint32_t lastInstanceCount_ = 0;

    // Compute pipeline for GPU frustum culling.
    VkPipeline cullPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout cullLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout cullSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool cullPool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> cullSets_; // per frame in flight

    // Instance storage buffers (SSBO): input + output per frame.
    VkBuffer instanceInBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory instanceInMemory_ = VK_NULL_HANDLE;
    void* instanceInMapped_ = nullptr;
    VkBuffer instanceOutBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory instanceOutMemory_ = VK_NULL_HANDLE;

    // Hi-Z downsample compute (hiz.comp): depth -> 512^2 R32F max-reduced.
    VkPipeline hizPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout hizLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout hizSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool hizPool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> hizSets_; // per frame in flight

    // Batch start offsets (host-visible, per frame): uvec2 per batch.
    VkBuffer batchRangeBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory batchRangeMemory_ = VK_NULL_HANDLE;
    void* batchRangeMapped_ = nullptr;

    // Skinning joints SSBO (set 4): host-visible, kMaxJoints mat4s per frame.
    std::vector<VkBuffer> jointBuffers_;
    std::vector<VkDeviceMemory> jointMemories_;
    std::vector<void*> jointMappeds_;

    VkDescriptorPool jointPool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> jointSets_; // per frame in flight

    std::function<void(VkCommandBuffer)> overlay_; // ImGui overlay draw hook

};

} // namespace engine
