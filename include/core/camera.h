#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace engine {

struct Camera {
    glm::vec3 position{0.0f, 8.0f, 12.0f};
    glm::vec3 rotation{glm::radians(-30.0f), 0.0f, 0.0f}; // radians, euler XYZ; pitch down at spawn

    float fov = 70.0f;        // degrees, vertical
    float aspect = 16.0f / 9.0f;
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;

    // View matrix from position + rotation (yaw around Y, pitch around X).
    [[nodiscard]] glm::mat4 viewMatrix() const {
        glm::mat4 rot = glm::rotate(glm::mat4(1.0f), rotation.y, glm::vec3(0, 1, 0));
        rot = glm::rotate(rot, rotation.x, glm::vec3(1, 0, 0));
        rot = glm::rotate(rot, rotation.z, glm::vec3(0, 0, 1));
        // World-to-view: inverse of camera transform.
        glm::vec4 forward = rot * glm::vec4(0, 0, -1, 0);
        return glm::lookAt(position, position + glm::vec3(forward), glm::vec3(0, 1, 0));
    }

    // Perspective projection, Vulkan-corrected: Y flip and Z range [0,1].
    [[nodiscard]] glm::mat4 projectionMatrix() const {
        glm::mat4 p = glm::perspective(glm::radians(fov), aspect, nearPlane, farPlane);
        p[1][1] *= -1.0f; // Vulkan clip space has +y down
        return p;
    }

    [[nodiscard]] glm::mat4 viewProjection() const { return projectionMatrix() * viewMatrix(); }
};

} // namespace engine
