#pragma once
#include <glm/glm.hpp>

namespace Engine {

struct DirectionalLightComponent {
    glm::vec3 direction{0.0f, -1.0f, -1.0f};
    glm::vec3 color{1.0f};
    float intensity{1.0f};
};

struct PointLightComponent {
    glm::vec3 position{0.0f};
    glm::vec3 color{1.0f};
    float intensity{1.0f};
    float radius{10.0f};
    float range{100.0f};
};

struct LightComponent {
    // Unified wrapper: type 0 = directional, 1 = point
    enum class Type { Directional, Point };
    Type type{Type::Directional};
    glm::vec3 direction{0.0f, -1.0f, -1.0f};
    glm::vec3 position{0.0f};
    glm::vec3 color{1.0f};
    float intensity{1.0f};
    float radius{10.0f};
};

} // namespace Engine

namespace engine {
    using DirectionalLightComponent = ::Engine::DirectionalLightComponent;
    using PointLightComponent = ::Engine::PointLightComponent;
    using LightComponent = ::Engine::LightComponent;
}
