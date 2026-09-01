#pragma once
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include <cstdint>

namespace Engine {

// Per-instance input uploaded to GPU
struct alignas(16) GPUInstanceData {
    glm::mat4 modelMatrix;
    glm::vec4 boundingSphere; // xyz: World-space center, w: Radius
    uint32_t meshID;          // Index into Mesh registry
    uint32_t materialID;      // Index into Material tables
    uint32_t batchIndex;      // Index into indirect draw command array
    uint32_t flags;
};

// Direct match with VkDrawIndexedIndirectCommand
struct alignas(16) GPUIndirectCommand {
    uint32_t indexCount{0};
    uint32_t instanceCount{0}; // Atomically incremented by culling shader
    uint32_t firstIndex{0};
    int32_t  vertexOffset{0};
    uint32_t firstInstance{0};
};

struct alignas(16) CullingUniforms {
    glm::mat4 viewMatrix;
    glm::mat4 projectionMatrix;
    glm::vec4 frustumPlanes[6]; // Frustum plane equations: Ax + By + Cz + D = 0
    glm::vec2 hizExtent;        // Top mip resolution (e.g., 1920, 1080)
    float zNear;
    float zFar;
    uint32_t totalInstances;
    uint32_t maxMipLevel;
};

} // namespace Engine

namespace engine {
    using GPUInstanceData = Engine::GPUInstanceData;
    using GPUIndirectCommand = Engine::GPUIndirectCommand;
    using CullingUniforms = Engine::CullingUniforms;
}
