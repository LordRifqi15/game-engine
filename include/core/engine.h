#pragma once

#include <memory>

#include "core/anim_editor.h"
#include "core/camera_controller.h"
#include "core/gameplay_graph.h"
#include "core/job_system.h"
#include "core/scene.h"
#include "core/time.h"
#include "core/world.h"
#include "modules/navigation/NavGrid.hpp"
#include "modules/physics/PhysicsSystem.hpp"

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
    void spawnFallbackScene();
    bool tryLoadScene(const std::string& path);

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
    // Task 035: gameplay graph editor (F2 to toggle) and per-entity binding
    std::unique_ptr<AnimGraphEditor> gameplayEditor_;
    EditorGraph gameplayEditorGraph_;
    bool gameplayEditorOpen_ = false;
    Entity gameplayEditorTarget_ = kInvalidEntity;
    // Task 036: player entity for NPC targeting
    Entity playerEntity_ = kInvalidEntity;
    // Task 037: physics system (velocity + gravity + ground)
    PhysicsSystem physicsSystem_;
    // Task 039: navigation grid (XZ plane)
    NavGrid navGrid_{32, 32, 1.0f, glm::vec3(-16.0f, 0.0f, -16.0f)};
};


} // namespace engine

