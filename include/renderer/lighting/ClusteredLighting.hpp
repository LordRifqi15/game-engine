#pragma once
#include "renderer/lighting/LightTypes.hpp"
#include "renderer/graph/RenderGraphResources.hpp"
#include "core/registry.h"
#include <vector>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace Engine {

class ClusteredLighting {
public:
    static constexpr uint32_t CLUSTER_TILE_SIZE = 64; // 64x64 pixel tiles
    static constexpr uint32_t CLUSTER_SLICES_Z = 24;  // 24 depth slices
    static constexpr uint32_t MAX_LIGHTS = 4096;
    static constexpr uint32_t MAX_LIGHTS_PER_CLUSTER = 128;

    void init(VkDevice device, VkPhysicalDevice physicalDevice);
    void updateLightBuffers(class ::engine::Registry& registry, const glm::mat4& viewMatrix, float zNear, float zFar, VkExtent2D extent);
    void updateLightBuffers(const std::vector<GPULight>& lights, const glm::mat4& viewMatrix, const glm::mat4& projMatrix, const glm::mat4& invProj, VkExtent2D extent, float zNear, float zFar);

    const ClusterUniforms& getUniforms() const { return uniforms_; }
    const std::vector<GPULight>& getGPULights() const { return cpuLights_; }
    uint32_t gridX() const { return uniforms_.gridDimensions.x; }
    uint32_t gridY() const { return uniforms_.gridDimensions.y; }
    uint32_t gridZ() const { return CLUSTER_SLICES_Z; }
    uint32_t totalClusters() const { return uniforms_.gridDimensions.w; }

    // CPU helpers for tests
    static void computeClusterAABB(uint32_t clusterIdx, const ClusterUniforms& u, glm::vec3& outMin, glm::vec3& outMax);
    static uint32_t computeGridX(VkExtent2D extent) { return (extent.width + CLUSTER_TILE_SIZE - 1) / CLUSTER_TILE_SIZE; }
    static uint32_t computeGridY(VkExtent2D extent) { return (extent.height + CLUSTER_TILE_SIZE - 1) / CLUSTER_TILE_SIZE; }

private:
    ClusterUniforms uniforms_{};
    std::vector<GPULight> cpuLights_;
    VkDevice device_{VK_NULL_HANDLE};
    VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
};

} // namespace Engine

namespace engine {
    using ClusteredLighting = ::Engine::ClusteredLighting;
}
