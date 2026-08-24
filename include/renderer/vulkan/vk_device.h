#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "renderer/api/render_device.h"

#include <cstdint>
#include <vector>

// Owns physical device pick, logical device, queues, command pool,
// command buffers, and per-frame fences.
namespace engine {

class VulkanInstance;

class VulkanDevice final : public RenderDevice {
public:
    explicit VulkanDevice(VulkanInstance& vk);
    ~VulkanDevice() override;

    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;

    // RenderDevice
    void waitIdle() override { vkDeviceWaitIdle(device_); }

    // Backend accessors for sibling classes (same module, allowed).
    VkPhysicalDevice physical() const { return physical_; }
    VkDevice handle() const { return device_; }
    VkQueue graphicsQueue() const { return graphicsQueue_; }
    VkCommandPool commandPool() const { return commandPool_; }
    VkQueue presentQueue() const { return presentQueue_; }
    uint32_t graphicsFamily() const { return graphicsFamily_; }
    uint32_t presentFamily() const { return presentFamily_; }

    struct QueueFamilies {
        int graphics = -1;
        int present = -1;
        bool complete() const { return graphics >= 0 && present >= 0; }
    };
    QueueFamilies findQueueFamilies(VkPhysicalDevice device) const;

    void createCommandPool();
    VkCommandBuffer allocateCommandBuffer();

    // CPU-side scratch buffers for mesh draws (temporary until GPU buffer pool).
    // ponytail: host-visible buffers created per draw; fine at this scale.
    // Scratch lifetime vs. frames in flight: buffers recorded into slot N's
    // command buffer are destroyed when slot N is re-acquired and its fence
    // has been waited — guaranteeing GPU completion.
    void retireScratchBuffers(uint32_t frameIndex);
    void flushRetiredScratch(uint32_t frameIndex);
    VkBuffer scratchVertexBuffer(const void* data, size_t bytes);
    VkBuffer scratchIndexBuffer(const void* data, size_t bytes);
    template <typename T>
    VkBuffer scratchVertexBuffer(const std::vector<T>& data) {
        return scratchVertexBuffer(data.data(), data.size() * sizeof(T));
    }
    template <typename T>
    VkBuffer scratchIndexBuffer(const std::vector<T>& data) {
        return scratchIndexBuffer(data.data(), data.size() * sizeof(T));
    }

    // Camera UBO descriptor set layout (set 0, binding 0, vertex stage).
    void createCameraDescriptorLayout();
    VkDescriptorSetLayout cameraDescriptorLayout() const { return cameraDescriptorLayout_; }

    // Material texture combined-image-sampler layout (set 1, binding 0, fragment).
    void createMaterialDescriptorLayout();
    VkDescriptorSetLayout materialDescriptorLayout() const { return materialDescriptorLayout_; }
    // Shadow map combined-image-sampler layout (set 2, binding 0, fragment).
    void createShadowSamplerLayout();
    VkDescriptorSetLayout shadowSamplerLayout() const { return shadowSamplerLayout_; }
    // IBL environment set (set 3): irradiance/prefiltered/BRDF/env cubemaps.
    void createSet3Layout();
    VkDescriptorSetLayout set3Layout() const { return set3Layout_; }

    // Persistent host-visible buffer (for camera UBO). One per frame-in-flight.
    void createUniformBuffers(uint32_t frameCount, VkDeviceSize size);
    VkBuffer uniformBuffer(uint32_t frameIndex) const { return uniformBuffers_[frameIndex]; }
    void* mapUniform(uint32_t frameIndex);   // persistently mapped
    void unmapUniform(uint32_t frameIndex);

    // Fences for frames in flight.
    void createFrameFences(uint32_t frameCount);
    VkFence fence(uint32_t frameIndex) const { return fences_[frameIndex]; }
    void resetFence(uint32_t frameIndex) { vkResetFences(device_, 1, &fences_[frameIndex]); }
    void waitForFence(uint32_t frameIndex) {
        vkWaitForFences(device_, 1, &fences_[frameIndex], VK_TRUE, UINT64_MAX);
    }

private:
    struct Support {
        VkSurfaceCapabilitiesKHR caps{};
        std::vector<VkSurfaceFormatKHR> formats;
        std::vector<VkPresentModeKHR> presentModes;
    };

    VulkanInstance& vk_;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;
    uint32_t graphicsFamily_ = 0;
    uint32_t presentFamily_ = 0;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkFence> fences_;

    // Scratch buffers created per draw; freed on destruction.
    std::vector<VkBuffer> scratchBuffers_;
    std::vector<VkDeviceMemory> scratchMemories_;

    // Retired scratch, one generation per frame-in-flight slot.
    static constexpr uint32_t kFrameSlots = 2;
    std::vector<VkBuffer> retiredBuffers_[kFrameSlots];
    std::vector<VkDeviceMemory> retiredMemories_[kFrameSlots];
    // Camera UBOs: one per frame-in-flight, persistently mapped.
    std::vector<VkBuffer> uniformBuffers_;
    std::vector<VkDeviceMemory> uniformMemories_;
    std::vector<void*> uniformMapped_;

    VkDescriptorSetLayout cameraDescriptorLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout materialDescriptorLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout shadowSamplerLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set3Layout_ = VK_NULL_HANDLE;
};

} // namespace engine
