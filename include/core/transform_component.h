#pragma once

#include <glm/glm.hpp>

// ECS component types: data-only structs.
namespace engine {

struct TransformComponent {
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f}; // radians
    glm::vec3 scale{1.0f};
};

} // namespace engine
