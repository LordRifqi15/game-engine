#include "modules/physics/PhysicsSystem.hpp"

#include "core/transform_component.h"
#include "ecs/components/PhysicsComponent.hpp"
#include "ecs/components/ColliderComponent.hpp"

#include <glm/glm.hpp>
#include <algorithm>

namespace Engine {

void PhysicsSystem::update(engine::Registry& registry, float dt) {
    const float GRAVITY = -9.81f;

    auto* transArr = registry.tryGetComponentArray<engine::TransformComponent>();
    auto* physArr = registry.tryGetComponentArray<engine::PhysicsComponent>();
    if (!transArr || !physArr) return;

    for (size_t i = 0; i < physArr->size(); ++i) {
        engine::Entity e = physArr->entityAt(i);
        if (!transArr->has(e)) continue;
        auto& transform = transArr->get(e);
        auto& phys = physArr->get(e);

        // 1. Apply gravity
        if (phys.useGravity && !phys.isGrounded) {
            phys.velocity.y += GRAVITY * dt;
        }

        // 2. Apply linear damping to horizontal (XZ)
        float dampingFactor = glm::clamp(1.0f - (phys.linearDamping * dt), 0.0f, 1.0f);
        phys.velocity.x *= dampingFactor;
        phys.velocity.z *= dampingFactor;

        // 3. Symplectic Euler integration
        transform.position += phys.velocity * dt;

        // 4. Ground plane constraint (y = 0 + offset)
        float groundOffset = 0.0f;
        auto* colArr = registry.tryGetComponentArray<engine::ColliderComponent>();
        if (colArr && colArr->has(e)) {
            const auto& col = colArr->get(e);
            groundOffset = (col.type == engine::ColliderType::Sphere) ? col.radius : col.halfExtents.y;
        }

        if (transform.position.y <= groundOffset) {
            transform.position.y = groundOffset;
            phys.velocity.y = 0.0f;
            phys.isGrounded = true;
        } else {
            phys.isGrounded = false;
        }
    }
}

} // namespace Engine
