#pragma once

#include "core/camera_controller.h"
#include "core/job_system.h"
#include "core/scene.h"
#include "core/time.h"

namespace engine {

class Window;
class Renderer;
class Scene;

// Engine core: owns frame loop, time, scene, renderer.
// Update = simulation. Render = visualization only.
class Engine {
public:
    explicit Engine(Window& window);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Runs until window closes: Update → Render each frame.
    void run();

    Scene& scene() { return *scene_; }

private:
    void update(double deltaTime);
    void render();

    Window& window_;
    Time time_;
    Renderer* renderer_ = nullptr;
    Scene* scene_ = nullptr;
    class Mesh* triangleMesh_ = nullptr;
    class Mesh* cubeMesh_ = nullptr;
    std::vector<class Mesh> gltfMeshes_;
    const class Texture* cubeTexture_ = nullptr; // opaque; owned by renderer
    CameraController controller_;
    JobSystem jobs_;
    bool debugTiming_ = false; // set true to log FPS once per second
};

} // namespace engine
