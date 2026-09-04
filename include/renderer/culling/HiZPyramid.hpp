#pragma once
#include "renderer/culling/CullingTypes.hpp"
#include "renderer/graph/RenderGraphResources.hpp"
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include <cmath>
#include <algorithm>

namespace Engine {

class RenderGraph;
struct ResourceHandle;

// Hi-Z image container & mip chain manager.
// CPU side mirrors the GPU pyramid: each mip is half-res max-depth.
class HiZPyramid {
public:
    HiZPyramid() = default;

    // Compute full mip chain length for screen extent (incl. 1x1 top)
    static uint32_t computeMipLevels(VkExtent2D extent) {
        uint32_t maxDim = std::max(extent.width, extent.height);
        if (maxDim == 0) return 1;
        return static_cast<uint32_t>(std::floor(std::log2(static_cast<float>(maxDim)))) + 1;
    }

    // Extent of mip `level` (0 = full res). Clamped to 1.
    static VkExtent2D mipExtent(VkExtent2D base, uint32_t level) {
        VkExtent2D e;
        e.width  = std::max(1u, base.width  >> level);
        e.height = std::max(1u, base.height >> level);
        return e;
    }

    // For Hi-Z pyramid spec: pyramid is stored at half-res (depth is downsampled once before chain)
    // So base pyramid extent is screen/2, but mipLevels computed from full screen.
    static VkExtent2D pyramidBaseExtent(VkExtent2D screenExtent) {
        return VkExtent2D{ std::max(1u, screenExtent.width / 2), std::max(1u, screenExtent.height / 2) };
    }

    // Create Hi-Z pyramid transient resource with full mip chain.
    static ResourceHandle createHiZResource(RenderGraph& graph, VkExtent2D screenExtent);

    // --- CPU helpers for occlusion_hiz_test (mirrors instance_cull.comp) ---

    // Extract frustum planes from view-projection matrix (row-major, 6 planes: left/right/bottom/top/near/far)
    // Returns normalized planes: Ax+By+Cz+D=0
    static void extractFrustumPlanes(const glm::mat4& viewProj, glm::vec4 outPlanes[6]);

    // Sphere vs frustum (conservative). Mirrors isFrustumVisible().
    static bool isSphereFrustumVisible(const glm::vec3& center, float radius, const glm::vec4 planes[6]);

    // Project bounding sphere to screen-space AABB in NDC [0,1]. Returns false if behind near or degenerate.
    // Uses same math as instance_cull.comp: ndcCenter + radius*P00 / -viewZ
    static bool projectSphereToAABB(const glm::vec3& worldCenter, float radius,
                                    const glm::mat4& view, const glm::mat4& proj,
                                    VkExtent2D screenExtent, float zNear,
                                    glm::vec2& outMin, glm::vec2& outMax, glm::vec2& outSizePixels);

    // Mip selection for AABB size in pixels: ceil(log2(maxDim)) clamped
    static uint32_t mipForAABBSize(const glm::vec2& sizePixels, uint32_t maxMip) {
        float maxDim = std::max(sizePixels.x, sizePixels.y);
        if (maxDim < 1.0f) maxDim = 1.0f;
        float mip = std::ceil(std::log2(maxDim));
        if (mip < 0) mip = 0;
        if (mip > float(maxMip)) mip = float(maxMip);
        return static_cast<uint32_t>(mip);
    }

    // Max-depth downsample of 2x2 footprint (mirrors hiz_build.comp). Conservative occluder uses max.
    static float downsampleMaxDepth(float d00, float d01, float d10, float d11) {
        return std::max(std::max(d00, d01), std::max(d10, d11));
    }

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
    VkExtent2D screenExtent_{0,0};
    uint32_t mipLevels_{0};
};

} // namespace Engine

namespace engine {
    using HiZPyramid = ::Engine::HiZPyramid;
}
