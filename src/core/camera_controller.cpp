#include "core/camera_controller.h"

#include "platform/input.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>

namespace engine {

void CameraController::update(Camera& camera, float dt) {
    // Mouse look.
    const float dx = Input::getMouseDeltaX();
    const float dy = Input::getMouseDeltaY();

    camera.rotation.y += dx * lookSpeed; // yaw
    camera.rotation.x -= dy * lookSpeed; // pitch (mouse up = look up)

    // Clamp pitch to avoid gimbal flip at the poles.
    constexpr float maxPitch = glm::radians(89.0f);
    camera.rotation.x = std::clamp(camera.rotation.x, -maxPitch, maxPitch);

    // Movement in camera plane (yaw only: fly-style, no vertical drift).
    const float yaw = camera.rotation.y;
    const glm::vec3 forward{-std::sin(yaw), 0.0f, -std::cos(yaw)};
    const glm::vec3 right{std::cos(yaw), 0.0f, -std::sin(yaw)};

    glm::vec3 move{0.0f};
    if (Input::isKeyPressed(GLFW_KEY_W)) move += forward;
    if (Input::isKeyPressed(GLFW_KEY_S)) move -= forward;
    if (Input::isKeyPressed(GLFW_KEY_A)) move -= right;
    if (Input::isKeyPressed(GLFW_KEY_D)) move += right;
    if (Input::isKeyPressed(GLFW_KEY_SPACE)) move.y += 1.0f;
    if (Input::isKeyPressed(GLFW_KEY_LEFT_SHIFT)) move.y -= 1.0f;

    // Normalize so diagonals aren't faster; dt makes it frame-rate independent.
    const float len = glm::length(move);
    if (len > 0.0f) {
        camera.position += (move / len) * moveSpeed * dt;
    }

    firstUpdate_ = false;
}

} // namespace engine
