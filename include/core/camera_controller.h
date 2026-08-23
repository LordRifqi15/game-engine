#pragma once

#include "core/camera.h"

// WASD + mouse-look controller. Frame-rate independent via dt.
namespace engine {

class CameraController {
public:
    void update(Camera& camera, float dt);

    float moveSpeed = 5.0f;      // units/second
    float lookSpeed = 0.1f;      // radians per pixel

private:
    bool firstUpdate_ = true;
};

} // namespace engine
