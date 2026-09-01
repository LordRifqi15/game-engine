#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include "renderer/material/MaterialTypes.hpp"

namespace Engine {

class BindlessDescriptorManager {
public:
    static constexpr uint32_t MAX_BINDLESS_TEXTURES = 16384;
    static constexpr uint32_t MAX_MATERIALS = 4096;

    BindlessDescriptorManager() = default;
    ~BindlessDescriptorManager() { if (device_ != VK_NULL_HANDLE) shutdown(); }

    void init(VkDevice device);
    void shutdown();

    // Texture slot registration
    uint32_t registerTexture(VkImageView imageView);
    void freeTexture(uint32_t slot);

    // Material buffer updating
    void updateMaterial(uint32_t materialID, const GPUMaterial& material);
    const GPUMaterial* getMaterial(uint32_t materialID) const;

    VkDescriptorSetLayout getDescriptorSetLayout() const { return descriptorSetLayout_; }
    VkDescriptorSet getDescriptorSet() const { return descriptorSet_; }
    VkBuffer getMaterialBuffer() const { return materialBuffer_; }

    // For tests
    uint32_t allocatedTextureCount() const { return allocatedCount_; }
    bool isTextureValid(uint32_t slot) const;

private:
    void initSamplers();
    void createDescriptorSetLayout();
    void allocateDescriptorSet();
    void createMaterialBuffer();

    VkDevice device_{VK_NULL_HANDLE};
    VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
    VkDescriptorSetLayout descriptorSetLayout_{VK_NULL_HANDLE};
    VkDescriptorSet descriptorSet_{VK_NULL_HANDLE};

    VkSampler staticSamplers_[static_cast<size_t>(SamplerType::Count)]{};

    VkBuffer materialBuffer_{VK_NULL_HANDLE};
    VkDeviceMemory materialMemory_{VK_NULL_HANDLE};
    std::vector<GPUMaterial> materialData_; // host copy, size MAX_MATERIALS

    std::vector<uint32_t> freeSlots_;
    std::vector<VkImageView> textureSlots_; // size MAX_BINDLESS_TEXTURES, for validation
    uint32_t allocatedCount_{0};
    uint32_t nextSlot_{0};
};

} // namespace Engine

namespace engine {
    using BindlessDescriptorManager = Engine::BindlessDescriptorManager;
}
