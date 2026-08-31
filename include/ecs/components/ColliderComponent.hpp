#pragma once
#include <glm/glm.hpp>

namespace Engine {

enum class ColliderType {
    Sphere,
    AABB
};

struct ColliderComponent {
    ColliderType type{ColliderType::Sphere};
    float radius{0.5f};
    glm::vec3 halfExtents{0.5f, 1.0f, 0.5f};
    glm::vec3 centerOffset{0.0f, 0.5f, 0.0f};
};

} // namespace Engine

namespace engine {
    using ColliderComponent = ::Engine::ColliderComponent;
    using ColliderType = ::Engine::ColliderType;
}
