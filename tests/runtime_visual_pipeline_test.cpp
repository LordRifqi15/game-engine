// Runtime Visual Pipeline: headless contract for the modern (SceneRenderer) path.
// Build: g++ -std=c++17 -I../include -I../third_party ../src/renderer/culling/HiZPyramid.cpp runtime_visual_pipeline_test.cpp -o /tmp/runtime_visual_pipeline_test -lvulkan && /tmp/runtime_visual_pipeline_test
//
// Covers: HiZ pyramid tail-mip writability (every dispatched mip must have extent
// >= 1x1 or the build shader early-outs and leaves zeros that over-cull), mip
// selection staying inside the written chain for a close large sphere, and the
// frustum keeping known-visible geometry (parity SoloCube at (0,1,-5)).

#include "renderer/culling/HiZPyramid.hpp"

#include <cstdio>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

using namespace Engine;

static int failCount = 0;
static void check(bool cond, const char* msg) {
    if (!cond) {
        printf("FAIL %s\n", msg);
        failCount++;
    } else {
        printf("OK %s\n", msg);
    }
}

// Mirrors SceneRenderer HiZ chain sizing: half-res base, +1 mip per halving, cap 11.
static uint32_t chainLength(VkExtent2D screen) {
    VkExtent2D base = HiZPyramid::pyramidBaseExtent(screen);
    uint32_t mips = 1;
    for (uint32_t d = std::max(base.width, base.height); d > 1; d >>= 1) {
        if (mips >= 11) break;
        ++mips;
    }
    return mips;
}

static glm::mat4 vulkanProj(float fovDeg, float aspect, float zNear, float zFar) {
    glm::mat4 p = glm::perspective(glm::radians(fovDeg), aspect, zNear, zFar);
    p[1][1] *= -1.0f;
    return p;
}

int main() {
    // 1. Tail mips dispatchable: every level in the chain resolves to >= 1x1.
    {
        const VkExtent2D screens[] = {{1280, 720}, {1920, 1080}, {640, 480}, {100, 100}};
        for (auto s : screens) {
            VkExtent2D base = HiZPyramid::pyramidBaseExtent(s);
            uint32_t mips = chainLength(s);
            bool allWritable = true;
            for (uint32_t m = 0; m < mips; ++m) {
                VkExtent2D e = HiZPyramid::mipExtent(base, m);
                allWritable &= (e.width >= 1 && e.height >= 1);
            }
            char msg[96];
            std::snprintf(msg, sizeof(msg), "HiZ all %ux%u mips writable (%u levels)", s.width,
                          s.height, mips);
            check(allWritable, msg);
        }
        // 720p regression: half-res height 360, level 9 must still be 1x1 (raw >> gives 0).
        VkExtent2D tail = HiZPyramid::mipExtent(HiZPyramid::pyramidBaseExtent({1280, 720}), 9);
        check(tail.width == 1 && tail.height == 1, "HiZ 720p tail mip 1x1");
    }

    // 2. Close large sphere selects a mip inside the written chain.
    {
        VkExtent2D screen{1280, 720};
        glm::mat4 view = glm::lookAt(glm::vec3(0, 8, 12), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
        glm::mat4 proj = vulkanProj(70.0f, 1280.0f / 720.0f, 0.1f, 1000.0f);
        glm::vec2 lo, hi, sizePx;
        bool ok = HiZPyramid::projectSphereToAABB(glm::vec3(0, 1, -5), 3.5f, view, proj, screen,
                                                  0.1f, lo, hi, sizePx);
        check(ok, "Cube sphere projects on screen");
        uint32_t mips = chainLength(screen);
        uint32_t mip = HiZPyramid::mipForAABBSize(sizePx, mips - 1);
        check(mip < mips, "Cube mip inside written chain");
    }

    // 3. Frustum keeps known-visible geometry, culls behind-camera.
    {
        glm::mat4 view = glm::lookAt(glm::vec3(0, 8, 12), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
        glm::mat4 proj = vulkanProj(70.0f, 1280.0f / 720.0f, 0.1f, 1000.0f);
        glm::vec4 planes[6];
        HiZPyramid::extractFrustumPlanes(proj * view, planes);
        check(HiZPyramid::isSphereFrustumVisible(glm::vec3(0, 1, -5), 3.5f, planes),
              "Frustum keeps parity cube");
        check(!HiZPyramid::isSphereFrustumVisible(glm::vec3(0, 8, 20), 1.0f, planes),
              "Frustum culls behind-camera sphere");
    }

    if (failCount == 0) printf("PASS runtime_visual_pipeline\n");
    return failCount == 0 ? 0 : 1;
}
