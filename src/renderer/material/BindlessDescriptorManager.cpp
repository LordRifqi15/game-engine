#include "renderer/material/BindlessDescriptorManager.hpp"
#include <stdexcept>
#include <cstring>

namespace Engine {

void BindlessDescriptorManager::init(VkDevice device) {
    device_ = device;
    materialData_.assign(MAX_MATERIALS, GPUMaterial{});
    textureSlots_.assign(MAX_BINDLESS_TEXTURES, VK_NULL_HANDLE);
    freeSlots_.clear();
    allocatedCount_ = 0;
    nextSlot_ = 0;

    // Reserve slots 0-3 for fallback textures (as per spec)
    // Headless: just mark them as allocated with dummy views
    for (uint32_t i = 0; i < 4; ++i) {
        textureSlots_[i] = reinterpret_cast<VkImageView>(0x100 + i);
    }
    nextSlot_ = 4;
    allocatedCount_ = 4;

    if (device_ == VK_NULL_HANDLE) {
        // Headless: dummy handles
        descriptorSetLayout_ = reinterpret_cast<VkDescriptorSetLayout>(0x2000);
        descriptorSet_ = reinterpret_cast<VkDescriptorSet>(0x2001);
        descriptorPool_ = reinterpret_cast<VkDescriptorPool>(0x2002);
        materialBuffer_ = reinterpret_cast<VkBuffer>(0x3000);
        materialMemory_ = reinterpret_cast<VkDeviceMemory>(0x3001);
        for (size_t i = 0; i < static_cast<size_t>(SamplerType::Count); ++i) {
            staticSamplers_[i] = reinterpret_cast<VkSampler>(0x4000 + i);
        }
        return;
    }

    initSamplers();
    createDescriptorSetLayout();
    allocateDescriptorSet();
    createMaterialBuffer();
    // Write fallback samplers to descriptor set
    // (real implementation would update descriptor set with samplers)
}

void BindlessDescriptorManager::shutdown() {
    if (device_ == VK_NULL_HANDLE) {
        descriptorSetLayout_ = VK_NULL_HANDLE;
        descriptorSet_ = VK_NULL_HANDLE;
        descriptorPool_ = VK_NULL_HANDLE;
        materialBuffer_ = VK_NULL_HANDLE;
        materialMemory_ = VK_NULL_HANDLE;
        for (auto& s : staticSamplers_) s = VK_NULL_HANDLE;
        device_ = VK_NULL_HANDLE;
        return;
    }
    if (materialBuffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, materialBuffer_, nullptr);
        materialBuffer_ = VK_NULL_HANDLE;
    }
    if (materialMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, materialMemory_, nullptr);
        materialMemory_ = VK_NULL_HANDLE;
    }
    for (auto& s : staticSamplers_) {
        if (s != VK_NULL_HANDLE) vkDestroySampler(device_, s, nullptr);
        s = VK_NULL_HANDLE;
    }
    if (descriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        descriptorPool_ = VK_NULL_HANDLE;
    }
    if (descriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
        descriptorSetLayout_ = VK_NULL_HANDLE;
    }
    descriptorSet_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
}

void BindlessDescriptorManager::initSamplers() {
    if (device_ == VK_NULL_HANDLE) return;
    VkSamplerCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter = VK_FILTER_LINEAR;
    ci.minFilter = VK_FILTER_LINEAR;
    ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ci.maxLod = VK_LOD_CLAMP_NONE;
    vkCreateSampler(device_, &ci, nullptr, &staticSamplers_[static_cast<size_t>(SamplerType::LinearRepeat)]);

    ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(device_, &ci, nullptr, &staticSamplers_[static_cast<size_t>(SamplerType::LinearClamp)]);

    ci.magFilter = VK_FILTER_NEAREST;
    ci.minFilter = VK_FILTER_NEAREST;
    ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    vkCreateSampler(device_, &ci, nullptr, &staticSamplers_[static_cast<size_t>(SamplerType::NearestRepeat)]);

    ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(device_, &ci, nullptr, &staticSamplers_[static_cast<size_t>(SamplerType::NearestClamp)]);

    // Shadow compare
    ci.magFilter = VK_FILTER_LINEAR;
    ci.minFilter = VK_FILTER_LINEAR;
    ci.compareEnable = VK_TRUE;
    ci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    vkCreateSampler(device_, &ci, nullptr, &staticSamplers_[static_cast<size_t>(SamplerType::ShadowCompare)]);
}

void BindlessDescriptorManager::createDescriptorSetLayout() {
    VkDescriptorSetLayoutBinding bindings[3]{};

    // Binding 0: Unbounded Sampled Image Array (texture2D globalTextures[])
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[0].descriptorCount = MAX_BINDLESS_TEXTURES;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

    // Binding 1: Static Samplers Array (sampler globalSamplers[])
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[1].descriptorCount = static_cast<uint32_t>(SamplerType::Count);
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

    // Binding 2: Material Storage Buffer (GPUMaterial materials[])
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorBindingFlags bindingFlags[3] = {
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT,
        0,
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT
    };

    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
    flagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flagsInfo.bindingCount = 3;
    flagsInfo.pBindingFlags = bindingFlags;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.pNext = &flagsInfo;
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.bindingCount = 3;
    layoutInfo.pBindings = bindings;

    if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &descriptorSetLayout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create bindless descriptor set layout");
    }
}

void BindlessDescriptorManager::allocateDescriptorSet() {
    VkDescriptorPoolSize poolSizes[3]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    poolSizes[0].descriptorCount = MAX_BINDLESS_TEXTURES;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;
    poolSizes[1].descriptorCount = static_cast<uint32_t>(SamplerType::Count);
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[2].descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = poolSizes;

    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create bindless descriptor pool");
    }

    VkDescriptorSetVariableDescriptorCountAllocateInfo variableInfo{};
    variableInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
    uint32_t variableCount = MAX_BINDLESS_TEXTURES;
    variableInfo.descriptorSetCount = 1;
    variableInfo.pDescriptorCounts = &variableCount;

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.pNext = &variableInfo;
    allocInfo.descriptorPool = descriptorPool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &descriptorSetLayout_;

    if (vkAllocateDescriptorSets(device_, &allocInfo, &descriptorSet_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate bindless descriptor set");
    }

    // Write static samplers
    VkDescriptorImageInfo samplerInfos[static_cast<size_t>(SamplerType::Count)];
    for (size_t i = 0; i < static_cast<size_t>(SamplerType::Count); ++i) {
        samplerInfos[i].sampler = staticSamplers_[i];
        samplerInfos[i].imageView = VK_NULL_HANDLE;
        samplerInfos[i].imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = descriptorSet_;
    w.dstBinding = 1;
    w.dstArrayElement = 0;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w.descriptorCount = static_cast<uint32_t>(SamplerType::Count);
    w.pImageInfo = samplerInfos;
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
}

void BindlessDescriptorManager::createMaterialBuffer() {
    VkDeviceSize size = sizeof(GPUMaterial) * MAX_MATERIALS;
    VkBufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size = size;
    ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &ci, nullptr, &materialBuffer_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create material buffer");
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, materialBuffer_, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    // Find host visible memory type (simplified: type 0)
    ai.memoryTypeIndex = 0;
    if (vkAllocateMemory(device_, &ai, nullptr, &materialMemory_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate material memory");
    }
    vkBindBufferMemory(device_, materialBuffer_, materialMemory_, 0);

    // Write descriptor for material buffer
    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = materialBuffer_;
    bufInfo.offset = 0;
    bufInfo.range = size;
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = descriptorSet_;
    w.dstBinding = 2;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.descriptorCount = 1;
    w.pBufferInfo = &bufInfo;
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
}

uint32_t BindlessDescriptorManager::registerTexture(VkImageView imageView) {
    if (imageView == VK_NULL_HANDLE) {
        // Fallback to slot 0 (white) for invalid handles
        return 0;
    }
    uint32_t slot = 0;
    if (!freeSlots_.empty()) {
        slot = freeSlots_.back();
        freeSlots_.pop_back();
    } else {
        if (nextSlot_ >= MAX_BINDLESS_TEXTURES) {
            throw std::runtime_error("Bindless texture slots exhausted");
        }
        slot = nextSlot_++;
    }
    textureSlots_[slot] = imageView;
    allocatedCount_++;

    if (device_ != VK_NULL_HANDLE) {
        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageView = imageView;
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptorSet_;
        write.dstBinding = 0;
        write.dstArrayElement = slot;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        write.descriptorCount = 1;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }
    return slot;
}

void BindlessDescriptorManager::freeTexture(uint32_t slot) {
    if (slot < 4) return; // Reserved slots 0-3 never freed
    if (slot >= MAX_BINDLESS_TEXTURES) return;
    if (textureSlots_[slot] == VK_NULL_HANDLE) return;
    textureSlots_[slot] = VK_NULL_HANDLE;
    freeSlots_.push_back(slot);
    if (allocatedCount_ > 0) allocatedCount_--;

    if (device_ != VK_NULL_HANDLE) {
        // Optionally clear descriptor (not required due to PARTIALLY_BOUND)
    }
}

void BindlessDescriptorManager::updateMaterial(uint32_t materialID, const GPUMaterial& material) {
    if (materialID >= MAX_MATERIALS) return;
    materialData_[materialID] = material;
    if (device_ != VK_NULL_HANDLE && materialBuffer_ != VK_NULL_HANDLE) {
        // Host visible: map and copy
        void* data = nullptr;
        vkMapMemory(device_, materialMemory_, materialID * sizeof(GPUMaterial), sizeof(GPUMaterial), 0, &data);
        if (data) {
            std::memcpy(data, &material, sizeof(GPUMaterial));
            vkUnmapMemory(device_, materialMemory_);
        }
    }
}

const GPUMaterial* BindlessDescriptorManager::getMaterial(uint32_t materialID) const {
    if (materialID >= MAX_MATERIALS) return nullptr;
    return &materialData_[materialID];
}

bool BindlessDescriptorManager::isTextureValid(uint32_t slot) const {
    if (slot >= MAX_BINDLESS_TEXTURES) return false;
    return textureSlots_[slot] != VK_NULL_HANDLE;
}

} // namespace Engine
