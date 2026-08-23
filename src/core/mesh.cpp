#include "core/mesh.h"

namespace engine {

namespace mesh_primitives {

Mesh triangle() {
    // Winding reversed vs. math-coords order so the triangle survives
    // VK_FRONT_FACE_COUNTER_CLOCKWISE + back-face culling in Vulkan's
    // framebuffer space (+y down).
    // Normal faces +z (toward default camera).
    Mesh m;
    const glm::vec3 n{0.0f, 0.0f, 1.0f};
    m.vertices = {
        {{0.0f, -0.5f, 0.0f}, n}, // top (red)
        {{-0.5f, 0.5f, 0.0f}, n}, // bottom-left (blue)
        {{0.5f, 0.5f, 0.0f}, n},  // bottom-right (green)
    };
    m.indices = {0, 1, 2};
    return m;
}

Mesh quad() {
    const glm::vec3 n{0.0f, 0.0f, 1.0f};
    Mesh m;
    m.vertices = {
        {{-0.5f, -0.5f, 0.0f}, n, {0.0f, 0.0f}},
        {{0.5f, -0.5f, 0.0f}, n, {1.0f, 0.0f}},
        {{0.5f, 0.5f, 0.0f}, n, {1.0f, 1.0f}},
        {{-0.5f, 0.5f, 0.0f}, n, {0.0f, 1.0f}},
    };
    m.indices = {0, 1, 2, 2, 3, 0};
    return m;
}

} // namespace mesh_primitives

} // namespace engine
