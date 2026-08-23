#pragma once

#include <glm/glm.hpp>

namespace engine {

// Bounding sphere for visibility culling.
struct BoundsComponent {
    glm::vec3 center{0.0f}; // local-space offset from entity position
    float radius = 1.0f;
};

} // namespace engine
