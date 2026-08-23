#pragma once

// Minimal transform. GLM types allowed here (math, not rendering backend).
#include <glm/glm.hpp>

namespace engine {

struct Transform {
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f}; // radians
    glm::vec3 scale{1.0f};
};

} // namespace engine
