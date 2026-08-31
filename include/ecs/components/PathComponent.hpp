#pragma once
#include <glm/glm.hpp>
#include <vector>

namespace Engine {

struct PathComponent {
    std::vector<glm::vec3> waypoints;
    size_t currentIndex{0};
    bool hasPath{false};
    bool isFinished{false};
    glm::vec3 destination{0.0f};

    void clear() {
        waypoints.clear();
        currentIndex = 0;
        hasPath = false;
        isFinished = false;
    }
};

} // namespace Engine

namespace engine {
    using PathComponent = ::Engine::PathComponent;
}
