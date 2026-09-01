#pragma once
#include <vulkan/vulkan.h>

namespace Engine {

void enableVulkan12Features(VkPhysicalDeviceVulkan12Features& features12);
bool checkDescriptorIndexingSupport(VkPhysicalDevice physicalDevice);

// Helper to query maxPerStageDescriptorSampledImages for clamping MAX_BINDLESS_TEXTURES
uint32_t queryMaxBindlessTextures(VkPhysicalDevice physicalDevice);

} // namespace Engine

namespace engine {
    using Engine::enableVulkan12Features;
    using Engine::checkDescriptorIndexingSupport;
    using Engine::queryMaxBindlessTextures;
}
