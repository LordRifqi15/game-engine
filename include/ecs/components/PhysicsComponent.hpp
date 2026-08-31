#pragma once
#include <glm/glm.hpp>

namespace Engine {

struct PhysicsComponent {
    glm::vec3 velocity{0.0f};
    glm::vec3 acceleration{0.0f};
    float mass{1.0f};
    float linearDamping{10.0f};
    bool useGravity{true};
    bool isGrounded{false};
};

} // namespace Engine

namespace engine {
    using PhysicsComponent = ::Engine::PhysicsComponent;
}
