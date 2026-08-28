#pragma once

#include <memory>

#include "core/anim_editor.h"
#include "core/camera_controller.h"
#include "core/job_system.h"
#include "core/scene.h"
#include "core/time.h"
#include "core/world.h"

#include "editor/anim_graph_editor.h"

namespace engine {

class Window;
class Renderer;
class Scene;

class Engine {
public:
    explicit Engine(Window& window);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

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
    const class Texture* cubeTexture_ = nullptr;
    CameraController controller_;
    World* world_ = nullptr;
    JobSystem jobs_;
    bool debugTiming_ = false;
    // Task 031: visual graph editor (F1 to toggle)
    std::unique_ptr<AnimGraphEditor> editor_;
    EditorGraph editorGraph_;
    Skeleton editorBaseSkeleton_;
    bool editorOpen_ = true;
};

} // namespace engine

