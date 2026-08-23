#pragma once

#include <glm/glm.hpp>
#include <vector>

namespace engine {

// Per-instance data for instanced draws. Layout matches the instance vertex
// binding: model matrix (4x vec4, locations 0-3... offset-based) + color.
struct InstanceData {
    glm::mat4 model;
    glm::vec4 color;    // rgb used, a spare
    glm::vec4 params;   // x = metallic, y = roughness, z/w unused
};

} // namespace engine
