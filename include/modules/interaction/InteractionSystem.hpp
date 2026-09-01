#pragma once
#include "core/registry.h"

namespace Engine {

class InteractionSystem {
public:
    void update(entt::registry& registry);
};

} // namespace Engine

namespace engine {
    using InteractionSystem = ::Engine::InteractionSystem;
}
