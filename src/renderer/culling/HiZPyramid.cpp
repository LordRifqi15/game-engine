#include "renderer/culling/HiZPyramid.hpp"
#include "renderer/graph/RenderGraph.hpp"
#include "renderer/graph/RenderGraphBuilder.hpp"
#include <glm/gtc/matrix_transform.hpp>

namespace Engine {

ResourceHandle HiZPyramid::createHiZResource(RenderGraph& graph, VkExtent2D screenExtent) {
    uint32_t mips = computeMipLevels(screenExtent);
    VkExtent2D base = pyramidBaseExtent(screenExtent);
    // Hi-Z pyramid is R32_SFLOAT, sampled + storage, full mip chain
    ImageDesc desc;
    desc.name = "HiZ_Pyramid";
    desc.format = VK_FORMAT_R32_SFLOAT;
    desc.extent = base;
    desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    desc.mipLevels = mips;
    return graph.createResource(desc);
}

void HiZPyramid::extractFrustumPlanes(const glm::mat4& viewProj, glm::vec4 outPlanes[6]) {
    // Extract rows for column-major glm (viewProj[col][row])
    glm::vec4 row0(viewProj[0][0], viewProj[1][0], viewProj[2][0], viewProj[3][0]);
    glm::vec4 row1(viewProj[0][1], viewProj[1][1], viewProj[2][1], viewProj[3][1]);
    glm::vec4 row2(viewProj[0][2], viewProj[1][2], viewProj[2][2], viewProj[3][2]);
    glm::vec4 row3(viewProj[0][3], viewProj[1][3], viewProj[2][3], viewProj[3][3]);
    // Vulkan RH_ZO depth 0..1: near = row2, far = row3 - row2
    outPlanes[0] = row3 + row0; // left
    outPlanes[1] = row3 - row0; // right
    outPlanes[2] = row3 + row1; // bottom
    outPlanes[3] = row3 - row1; // top
    outPlanes[4] = row2;        // near
    outPlanes[5] = row3 - row2; // far
    for (int i = 0; i < 6; ++i) {
        float len = glm::length(glm::vec3(outPlanes[i]));
        if (len > 1e-6f) outPlanes[i] /= len;
    }
}

bool HiZPyramid::isSphereFrustumVisible(const glm::vec3& center, float radius, const glm::vec4 planes[6]) {
    for (int i = 0; i < 6; ++i) {
        if (glm::dot(glm::vec3(planes[i]), center) + planes[i].w < -radius) return false;
    }
    return true;
}

bool HiZPyramid::projectSphereToAABB(const glm::vec3& worldCenter, float radius,
                                     const glm::mat4& view, const glm::mat4& proj,
                                     VkExtent2D screenExtent, float zNear,
                                     glm::vec2& outMin, glm::vec2& outMax, glm::vec2& outSizePixels) {
    // Mimic instance_cull.comp
    glm::vec4 viewCenterH = view * glm::vec4(worldCenter, 1.0f);
    glm::vec3 viewCenter = glm::vec3(viewCenterH);
    if (viewCenter.z + radius >= -zNear) {
        // Behind or intersecting near plane -> full screen, conservatively visible
        outMin = glm::vec2(0.0f);
        outMax = glm::vec2(1.0f);
        outSizePixels = glm::vec2(float(screenExtent.width), float(screenExtent.height));
        return true;
    }
    glm::vec4 clipCenter = proj * glm::vec4(viewCenter, 1.0f);
    if (clipCenter.w == 0.0f) return false;
    glm::vec2 ndcCenter = glm::vec2(clipCenter.x / clipCenter.w, clipCenter.y / clipCenter.w) * 0.5f + 0.5f;
    float P00 = proj[0][0];
    float P11 = proj[1][1];
    float radiusX = (radius * P00) / -viewCenter.z;
    float radiusY = (radius * P11) / -viewCenter.z;
    glm::vec2 aabbMin = glm::clamp(ndcCenter - glm::vec2(radiusX, radiusY) * 0.5f, glm::vec2(0.0f), glm::vec2(1.0f));
    glm::vec2 aabbMax = glm::clamp(ndcCenter + glm::vec2(radiusX, radiusY) * 0.5f, glm::vec2(0.0f), glm::vec2(1.0f));
    outMin = aabbMin;
    outMax = aabbMax;
    outSizePixels = (aabbMax - aabbMin) * glm::vec2(float(screenExtent.width), float(screenExtent.height));
    // Degenerate if fully off-screen
    if (outSizePixels.x <= 0.0f || outSizePixels.y <= 0.0f) return false;
    return true;
}

} // namespace Engine
