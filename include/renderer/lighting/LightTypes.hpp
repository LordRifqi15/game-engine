#pragma once
#include <glm/glm.hpp>
#include <cstdint>

namespace Engine {

enum class LightType : uint32_t {
    Directional = 0,
    Point = 1,
    Spot = 2
};

// Align to 16 bytes for std430 SSBO packing
struct alignas(16) GPULight {
    glm::vec4 positionRadius;    // xyz: View-space or World-space pos, w: Radius
    glm::vec4 colorIntensity;    // rgb: Color, w: Intensity (lux / lumens)
    glm::vec4 directionAngle;    // xyz: Normalized direction, w: Spot cutoff cosine
    uint32_t type;               // 0: Dir, 1: Point, 2: Spot
    uint32_t castsShadows;       // 1 = True, 0 = False
    uint32_t shadowMapIndex;     // Index into shadow atlas / cascade array
    float padding;
};

struct ClusterCell {
    uint32_t offset;             // Start index in the global light index list
    uint32_t pointLightCount;    // Number of point/spot lights in this cluster
    // pad to 8? spec shows no pad, keep 8 bytes
};

struct alignas(16) ClusterUniforms {
    glm::mat4 viewMatrix;
    glm::mat4 projectionMatrix;
    glm::mat4 inverseProjection;
    glm::uvec4 gridDimensions;   // x: gridX (e.g., 16), y: gridY (e.g., 9), z: gridZ (e.g., 24), w: totalClusters
    glm::vec4 screenDimensions;  // x: screenWidth, y: screenHeight, z: zNear, w: zFar
};

// Helpers for CPU math (tested)
inline uint32_t sliceIndex(float z, float zNear, float zFar, uint32_t slicesZ) {
    if (z < zNear) z = zNear;
    if (z > zFar) z = zFar;
    // clamp log to avoid NaN
    float ratio = z / zNear;
    float logRatio = logf(ratio);
    float logFarNear = logf(zFar / zNear);
    float f = logRatio * float(slicesZ) / logFarNear;
    int32_t idx = int32_t(floorf(f));
    if (idx < 0) idx = 0;
    if (idx >= int32_t(slicesZ)) idx = int32_t(slicesZ) - 1;
    return uint32_t(idx);
}

inline bool testSphereAABB(const glm::vec3& center, float radius, const glm::vec3& minAABB, const glm::vec3& maxAABB) {
    float sqDist = 0.0f;
    for (int i = 0; i < 3; ++i) {
        float v = center[i];
        if (v < minAABB[i]) sqDist += (minAABB[i] - v) * (minAABB[i] - v);
        if (v > maxAABB[i]) sqDist += (v - maxAABB[i]) * (v - maxAABB[i]);
    }
    return sqDist <= (radius * radius);
}

inline uint32_t getClusterIndex(glm::vec2 screenUV, float viewZ, const ClusterUniforms& u) {
    // screenUV in [0,1], viewZ negative (view space). Use abs.
    float vz = fabsf(viewZ);
    if (vz < u.screenDimensions.z) vz = u.screenDimensions.z;
    uint32_t tilesX = u.gridDimensions.x;
    uint32_t tilesY = u.gridDimensions.y;
    uint32_t slicesZ = u.gridDimensions.z;
    uint32_t tileX = uint32_t(screenUV.x * float(tilesX));
    uint32_t tileY = uint32_t(screenUV.y * float(tilesY));
    if (tileX >= tilesX) tileX = tilesX - 1;
    if (tileY >= tilesY) tileY = tilesY - 1;
    uint32_t sliceZ = sliceIndex(vz, u.screenDimensions.z, u.screenDimensions.w, slicesZ);
    return sliceZ * (tilesX * tilesY) + tileY * tilesX + tileX;
}

} // namespace Engine

namespace engine {
    using LightType = ::Engine::LightType;
    using GPULight = ::Engine::GPULight;
    using ClusterCell = ::Engine::ClusterCell;
    using ClusterUniforms = ::Engine::ClusterUniforms;
}
