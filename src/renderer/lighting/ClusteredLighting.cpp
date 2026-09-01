#include "renderer/lighting/ClusteredLighting.hpp"
#include "renderer/scene/LightComponent.hpp"
#include "core/registry.h"
#include <glm/gtc/matrix_inverse.hpp>
#include <cmath>
#include <algorithm>

namespace Engine {

void ClusteredLighting::init(VkDevice device, VkPhysicalDevice physicalDevice) {
    device_ = device;
    physicalDevice_ = physicalDevice;
    // No actual Vulkan allocation in headless; uniforms zeroed
    uniforms_ = {};
}

void ClusteredLighting::updateLightBuffers(::engine::Registry& registry, const glm::mat4& viewMatrix, float zNear, float zFar, VkExtent2D extent) {
    // Default projection for CPU test: perspective 60 deg
    float aspect = extent.width ? float(extent.width)/float(extent.height) : 1.0f;
    float fov = glm::radians(60.0f);
    float tanHalf = tanf(fov * 0.5f);
    glm::mat4 proj(0.0f);
    proj[0][0] = 1.0f/(aspect*tanHalf);
    proj[1][1] = 1.0f/tanHalf;
    proj[2][2] = zFar/(zFar - zNear);
    proj[2][3] = 1.0f;
    proj[3][2] = -(zFar*zNear)/(zFar - zNear);
    glm::mat4 invProj = glm::inverse(proj);
    std::vector<GPULight> lights;
    lights.reserve(MAX_LIGHTS);
    // Gather from registry: PointLightComponent and LightComponent point
    if (auto* arr = registry.tryGetComponentArray<PointLightComponent>()) {
        for (size_t i = 0; i < arr->size(); ++i) {
            if (lights.size() >= MAX_LIGHTS) break;
            auto e = arr->entityAt(i);
            auto& pl = arr->get(e);
            GPULight g{};
            g.positionRadius = glm::vec4(pl.position, pl.radius);
            g.colorIntensity = glm::vec4(pl.color, pl.intensity);
            g.directionAngle = glm::vec4(0,0,0,0);
            g.type = uint32_t(LightType::Point);
            g.castsShadows = 0;
            g.shadowMapIndex = 0;
            g.padding = 0;
            lights.push_back(g);
        }
    }
    if (auto* arr = registry.tryGetComponentArray<LightComponent>()) {
        for (size_t i = 0; i < arr->size(); ++i) {
            if (lights.size() >= MAX_LIGHTS) break;
            auto e = arr->entityAt(i);
            auto& lc = arr->get(e);
            if (lc.type != LightComponent::Type::Point) continue;
            // avoid duplicate if already added via PointLightComponent? keep both
            GPULight g{};
            g.positionRadius = glm::vec4(lc.position, lc.radius);
            g.colorIntensity = glm::vec4(lc.color, lc.intensity);
            g.directionAngle = glm::vec4(0,0,0,0);
            g.type = uint32_t(LightType::Point);
            g.castsShadows = 0;
            g.shadowMapIndex = 0;
            g.padding = 0;
            lights.push_back(g);
        }
    }
    updateLightBuffers(lights, viewMatrix, proj, invProj, extent, zNear, zFar);
}

void ClusteredLighting::updateLightBuffers(const std::vector<GPULight>& lights, const glm::mat4& viewMatrix, const glm::mat4& projMatrix, const glm::mat4& invProj, VkExtent2D extent, float zNear, float zFar) {
    uint32_t gx = computeGridX(extent);
    uint32_t gy = computeGridY(extent);
    uint32_t gz = CLUSTER_SLICES_Z;
    uint32_t total = gx * gy * gz;
    cpuLights_ = lights;
    if (cpuLights_.size() > MAX_LIGHTS) cpuLights_.resize(MAX_LIGHTS);
    uniforms_.viewMatrix = viewMatrix;
    uniforms_.projectionMatrix = projMatrix;
    uniforms_.inverseProjection = invProj;
    uniforms_.gridDimensions = glm::uvec4(gx, gy, gz, total);
    uniforms_.screenDimensions = glm::vec4(float(extent.width), float(extent.height), zNear, zFar);
}

void ClusteredLighting::computeClusterAABB(uint32_t clusterIdx, const ClusterUniforms& u, glm::vec3& outMin, glm::vec3& outMax) {
    uint32_t tilesX = u.gridDimensions.x;
    uint32_t tilesY = u.gridDimensions.y;
    uint32_t slicesZ = u.gridDimensions.z;
    float screenW = u.screenDimensions.x;
    float screenH = u.screenDimensions.y;
    float zNear = u.screenDimensions.z;
    float zFar = u.screenDimensions.w;
    if (clusterIdx >= tilesX * tilesY * slicesZ) {
        outMin = outMax = glm::vec3(0);
        return;
    }
    uint32_t sliceZ = clusterIdx / (tilesX * tilesY);
    uint32_t rem = clusterIdx % (tilesX * tilesY);
    uint32_t tileY = rem / tilesX;
    uint32_t tileX = rem % tilesX;

    // Log depth slices
    float zTileNear = zNear * powf(zFar / zNear, float(sliceZ) / float(slicesZ));
    float zTileFar  = zNear * powf(zFar / zNear, float(sliceZ + 1) / float(slicesZ));

    // Tile bounds in screen space
    float tileSizeX = screenW / float(tilesX);
    float tileSizeY = screenH / float(tilesY);
    float screenMinX = float(tileX) * tileSizeX;
    float screenMaxX = screenMinX + tileSizeX;
    float screenMinY = float(tileY) * tileSizeY;
    float screenMaxY = screenMinY + tileSizeY;

    // Convert screen to NDC [-1,1]
    float ndcMinX = (screenMinX / screenW) * 2.0f - 1.0f;
    float ndcMaxX = (screenMaxX / screenW) * 2.0f - 1.0f;
    float ndcMinY = (screenMinY / screenH) * 2.0f - 1.0f;
    float ndcMaxY = (screenMaxY / screenH) * 2.0f - 1.0f;

    // Use inverse projection to get view-space extents if available; otherwise approximate with linear scale
    // We approximate view X = NDC * depth * aspectScale, Y = NDC * depth *scale
    // Derive scale from projection matrix if non-zero: proj[0][0] = 1/(aspect*tan), proj[1][1]=1/tan
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    if (fabsf(u.projectionMatrix[0][0]) > 1e-6f) scaleX = 1.0f / u.projectionMatrix[0][0];
    if (fabsf(u.projectionMatrix[1][1]) > 1e-6f) scaleY = 1.0f / u.projectionMatrix[1][1];
    // If projection is identity (tests), scale defaults to 1

    // View space: z is negative in view (camera looks -Z), but we store positive depth for AABB; use negative for view
    // Compute min/max view positions at near and far depths
    float vxMinNear = ndcMinX * zTileNear * scaleX;
    float vxMaxNear = ndcMaxX * zTileNear * scaleX;
    float vyMinNear = ndcMinY * zTileNear * scaleY;
    float vyMaxNear = ndcMaxY * zTileNear * scaleY;
    float vxMinFar = ndcMinX * zTileFar * scaleX;
    float vxMaxFar = ndcMaxX * zTileFar * scaleX;
    float vyMinFar = ndcMinY * zTileFar * scaleY;
    float vyMaxFar = ndcMaxY * zTileFar * scaleY;

    float minX = std::min({vxMinNear, vxMaxNear, vxMinFar, vxMaxFar});
    float maxX = std::max({vxMinNear, vxMaxNear, vxMinFar, vxMaxFar});
    float minY = std::min({vyMinNear, vyMaxNear, vyMinFar, vyMaxFar});
    float maxY = std::max({vyMinNear, vyMaxNear, vyMinFar, vyMaxFar});
    // Z in view space: negative, but for AABB test we use positive depth convention (lightViewPos is transformed by view matrix, which gives negative Z for in-front)
    // To make test simple, we store AABB in view space with negative Z, so sphere test uses view-space center (also negative Z)
    float minZ = -zTileFar; // more negative = farther
    float maxZ = -zTileNear; // less negative = nearer

    outMin = glm::vec3(minX, minY, minZ);
    outMax = glm::vec3(maxX, maxY, maxZ);

    // Ensure min <= max (handle sign)
    if (outMin.x > outMax.x) std::swap(outMin.x, outMax.x);
    if (outMin.y > outMax.y) std::swap(outMin.y, outMax.y);
    if (outMin.z > outMax.z) std::swap(outMin.z, outMax.z);
}

} // namespace Engine
