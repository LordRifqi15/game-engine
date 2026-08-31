#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cstdint>

namespace engine {

struct GraphContext {
    uint32_t selfEntity{0};
    uint32_t targetEntity{0};
    glm::vec3 selfPosition{0.0f};
    glm::vec3 targetPosition{0.0f};
    glm::vec3* outSelfPosition{nullptr};
    glm::quat* outSelfRotation{nullptr};
    // Extension for engine's TransformComponent which stores Euler vec3 rotation
    glm::vec3* outSelfRotationEuler{nullptr};
    float dt{0.0f};
};

} // namespace engine

// Alias for spec's Engine namespace (capital E)
namespace Engine {
    using GraphContext = engine::GraphContext;
}
