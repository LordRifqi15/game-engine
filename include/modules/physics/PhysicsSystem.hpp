#pragma once
#include "core/registry.h"

namespace Engine {

class PhysicsSystem {
public:
    void update(class ::engine::Registry& registry, float dt);
};

} // namespace Engine

namespace engine {
    using PhysicsSystem = ::Engine::PhysicsSystem;
}
