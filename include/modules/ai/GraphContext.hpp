#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "ecs/components/PhysicsComponent.hpp"
#include "ecs/components/BlackboardComponent.hpp"
#include "ecs/components/PathComponent.hpp"
#include "modules/navigation/NavGrid.hpp"
#include <cstdint>

namespace engine {
struct GraphContext {
    uint32_t selfEntity{0};
    uint32_t targetEntity{0};
    glm::vec3 selfPosition{0.0f};
    glm::vec3 targetPosition{0.0f};
    PhysicsComponent* outPhysics{nullptr};
    BlackboardComponent* blackboard{nullptr};
    PathComponent* path{nullptr};
    const NavGrid* navGrid{nullptr};
    glm::quat* outSelfRotation{nullptr};
    float dt{0.0f};
    // Legacy direct position mutation (Task 036) — kept for backward compat
    glm::vec3* outSelfPosition{nullptr};
    glm::vec3* outSelfRotationEuler{nullptr};
};

} // namespace engine

// Alias for spec's Engine namespace (capital E)
namespace Engine {
    using GraphContext = engine::GraphContext;
    using PhysicsComponent = engine::PhysicsComponent;
    using BlackboardComponent = engine::BlackboardComponent;
    using PathComponent = engine::PathComponent;
    using NavGrid = engine::NavGrid;
}
