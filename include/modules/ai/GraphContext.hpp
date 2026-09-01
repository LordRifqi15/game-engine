#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "ecs/components/PhysicsComponent.hpp"
#include "ecs/components/BlackboardComponent.hpp"
#include "ecs/components/PathComponent.hpp"
#include "modules/navigation/NavGrid.hpp"
#include "modules/interaction/Event.hpp"
#include <vector>
#include <cstdint>

namespace engine {
class Registry;
struct GraphContext {
    uint32_t selfEntity{0};
    uint32_t targetEntity{0};
    glm::vec3 selfPosition{0.0f};
    glm::vec3 targetPosition{0.0f};
    PhysicsComponent* outPhysics{nullptr};
    BlackboardComponent* blackboard{nullptr};
    PathComponent* path{nullptr};
    const NavGrid* navGrid{nullptr};
    const std::vector<Event>* incomingEvents{nullptr};
    Registry* registry{nullptr};
    glm::quat* outSelfRotation{nullptr};
    float dt{0.0f};
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
