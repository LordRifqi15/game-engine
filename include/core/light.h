#pragma once

#include <glm/glm.hpp>

namespace engine {

struct DirectionalLight {
    glm::vec3 direction{0.0f, -1.0f, -1.0f}; // direction light travels
    glm::vec3 color{1.0f};
};

} // namespace engine
