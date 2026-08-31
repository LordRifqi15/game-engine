#pragma once
#include "modules/navigation/NavGrid.hpp"
#include <glm/glm.hpp>
#include <vector>

namespace Engine {

class Pathfinding {
public:
    static std::vector<glm::vec3> findPath(
        const NavGrid& grid,
        const glm::vec3& startWorld,
        const glm::vec3& targetWorld
    );
};

} // namespace Engine

namespace engine {
    using Pathfinding = ::Engine::Pathfinding;
}
