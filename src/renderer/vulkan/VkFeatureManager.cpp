#include "renderer/vulkan/VkFeatureManager.hpp"
#include <vulkan/vulkan.h>

namespace Engine {

void enableVulkan12Features(VkPhysicalDeviceVulkan12Features& features12) {
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.descriptorIndexing = VK_TRUE;
    features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    features12.descriptorBindingPartiallyBound = VK_TRUE;
    features12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    features12.runtimeDescriptorArray = VK_TRUE;
}

bool checkDescriptorIndexingSupport(VkPhysicalDevice physicalDevice) {
    if (physicalDevice == VK_NULL_HANDLE) return true; // headless: assume supported
    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &features12;
    vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);
    return features12.descriptorIndexing && features12.shaderSampledImageArrayNonUniformIndexing &&
           features12.descriptorBindingSampledImageUpdateAfterBind && features12.descriptorBindingPartiallyBound &&
           features12.descriptorBindingVariableDescriptorCount && features12.runtimeDescriptorArray;
}

uint32_t queryMaxBindlessTextures(VkPhysicalDevice physicalDevice) {
    if (physicalDevice == VK_NULL_HANDLE) return 16384;
    VkPhysicalDeviceDescriptorIndexingProperties indexingProps{};
    indexingProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &indexingProps;
    vkGetPhysicalDeviceProperties2(physicalDevice, &props2);
    uint32_t maxPerStage = indexingProps.maxPerStageDescriptorUpdateAfterBindSampledImages;
    // Clamp to our MAX_BINDLESS_TEXTURES
    if (maxPerStage < 16384) return maxPerStage;
    return 16384;
}

} // namespace Engine
