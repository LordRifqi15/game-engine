#include "core/scene/Scene.hpp"
#include "core/scene.h"

// Scene is header-only via engine::Scene; this file exists for task compliance
// and to ensure the scene module has a compilation unit.

namespace Engine {
    // Engine::Scene is alias to ::engine::Scene, no separate implementation needed
}

namespace engine {
    // engine::Scene is defined in core/scene.h
}
