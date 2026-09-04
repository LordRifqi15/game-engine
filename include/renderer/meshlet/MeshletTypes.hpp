#pragma once
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include <cstdint>

namespace Engine {

static constexpr uint32_t MESHLET_MAX_VERTICES = 64;
static constexpr uint32_t MESHLET_MAX_TRIANGLES = 124;

// 64-byte std430 aligned structure
struct alignas(16) GPUMeshlet {
    glm::vec4 boundingSphere;     // xyz: World/Local center, w: Radius
    glm::vec4 normalCone;         // xyz: Normal axis, w: sin(cutoffAngle)
    
    uint32_t vertexOffset;        // Offset into global vertex indirection buffer
    uint32_t vertexCount;
    uint32_t triangleOffset;      // Offset into global local-index buffer (packed uint8_t)
    uint32_t triangleCount;
    uint32_t padding[4]{};        // Pad to 64 bytes (48 -> 64)
};

// Global metadata per mesh instance
struct alignas(16) GPUMeshInstance {
    glm::mat4 modelMatrix;
    uint32_t meshletOffset;       // Index of first meshlet in global meshlet SSBO
    uint32_t meshletCount;
    uint32_t materialID;          // Bindless material table ID
    uint32_t flags;
};

struct alignas(16) MeshletCullUniforms {
    glm::mat4 viewMatrix;
    glm::mat4 projectionMatrix;
    glm::vec4 frustumPlanes[6];
    glm::vec3 cameraWorldPos;
    float zNear;
    glm::vec2 hizExtent;
    uint32_t maxMipLevel;
    uint32_t totalInstances;
};

// Also alias for meshlet count (same layout, different semantic)
static_assert(sizeof(GPUMeshlet) == 64, "GPUMeshlet must be 64 bytes");
static_assert(sizeof(GPUMeshInstance) == 80, "GPUMeshInstance must be 80 bytes (mat4 64 + 16)");

} // namespace Engine

namespace engine {
    using GPUMeshlet = ::Engine::GPUMeshlet;
    using GPUMeshInstance = ::Engine::GPUMeshInstance;
    using MeshletCullUniforms = ::Engine::MeshletCullUniforms;
}
