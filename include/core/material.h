#pragma once

#include <glm/glm.hpp>

namespace engine {

// Opaque handle: engine layer never touches Vulkan types.
class Texture;

struct Material {
    glm::vec4 baseColor{1.0f};
    const Texture* baseColorTexture = nullptr; // optional; null = solid baseColor

    float metallic = 0.0f;   // stored for future PBR
    float roughness = 1.0f;  // stored for future PBR
};

} // namespace engine
