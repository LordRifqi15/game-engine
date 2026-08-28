#include "core/engine.h"

#include "core/anim_graph.h"
#include "core/anim_state_machine.h"
#include "core/animation_system.h"
#include "core/bounds_component.h"
#include "core/mesh.h"
#include "core/gltf_loader.h"
#include "core/skeleton.h"
#include "core/mesh_component.h"
#include "core/material_component.h"
#include "core/transform_component.h"
#include "core/mesh_loader.h"
#include "platform/input.h"
#include "platform/window.h"
#include "renderer/renderer.h"
#include "core/render_system.h"

#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <thread>
#include <algorithm>
#include <cstdlib>

namespace engine {

Engine::Engine(Window& window)
    : window_(window) {
    renderer_ = new Renderer(window_);
    renderer_->enableEditorOverlay(window_);
    scene_ = new Scene();

        // Streaming world: shared quad mesh for ground chunks, rotated to XZ plane.
    triangleMesh_ = new Mesh(mesh_primitives::quad());
    world_ = new World(scene_->registry(), triangleMesh_, 2, 16.0f);

    // Try to load animated SimpleSkin model for skeletal demo (Option A: one skeleton per draw)
    {
        std::string gltfPath = "/home/lordrifqi15/Documents/code/project/pribadi/game-engine/assets/models/SimpleSkin/SimpleSkin.gltf";
        GLTFModel gltf = loadGLTFModel(gltfPath, &renderer_->textureCache());
        std::printf("[engine] loadGLTFModel returned ok=%d primitives=%zu skeletons=%zu anims=%zu error='%s'\n", gltf.ok, gltf.primitives.size(), gltf.skeletons.size(), gltf.animations.size(), gltf.error.c_str()); std::fflush(stdout);
        if (gltf.ok && !gltf.primitives.empty() && !gltf.skeletons.empty()) {
            std::printf("[engine] loaded SimpleSkin: %zu primitives, %zu joints, %zu anims\n",
                        gltf.primitives.size(), gltf.skeletons[0].joints.size(), gltf.animations.size()); std::fflush(stdout);
            for (auto& prim : gltf.primitives) {
                gltfMeshes_.push_back(std::move(prim.mesh));
                Mesh* meshPtr = &gltfMeshes_.back();
                Entity e = scene_->registry().createEntity();
                TransformComponent tc;
                tc.position = glm::vec3(0.0f, 0.0f, 0.0f);
                tc.scale = glm::vec3(1.0f);
                scene_->registry().addComponent<TransformComponent>(e, tc);
                MeshComponent mc; mc.mesh = meshPtr;
                scene_->registry().addComponent<MeshComponent>(e, mc);
                MaterialComponent matC; matC.material = prim.material;
                scene_->registry().addComponent<MaterialComponent>(e, matC);
                SkeletonComponent skelC; skelC.skeleton = gltf.skeletons[0];
                scene_->registry().addComponent<SkeletonComponent>(e, skelC);
                if (!gltf.animations.empty()) {
                    AnimationComponent animC;
                    animC.animations = gltf.animations;
                    if (animC.animations.size() == 1) {
                        auto copy = animC.animations[0];
                        copy.name = copy.name.empty() ? "Copy" : copy.name + "_Copy";
                        animC.animations.push_back(std::move(copy));
                        std::printf("[engine] duplicated anim for blending demo: now %zu anims\n", animC.animations.size());
                        std::fflush(stdout);
                    }
                    animC.speed = 1.0f;
                    animC.playing = true;
                    animC.loop = true;
                    // Task 030: state machine selects graphs; graphs select poses.
                    float jumpDur = animC.animations[1].duration > 0.0f ? animC.animations[1].duration : 1.0f;
                    animC.machine = makeDefaultStateMachine(animC.animations, skelC.skeleton, jumpDur);
                    std::printf("[state] machine built: %zu states (Idle/Locomotion/Jump)\n", animC.animations.size());
                    std::fflush(stdout);
                    // Task 031: editor mirrors locomotion graph for visual editing
                    if (!editor_) {
                        editorGraph_ = makeLocomotionEditorGraph(animC.animations);
                        editorBaseSkeleton_ = skelC.skeleton;
                        editor_ = std::make_unique<AnimGraphEditor>(editorGraph_);
                        std::printf("[editor] graph editor ready: %zu nodes, output %d\n", editorGraph_.nodes.size(), editorGraph_.outputNode);
                        std::fflush(stdout);
                    }
                    scene_->registry().addComponent<AnimationComponent>(e, std::move(animC));
                }
            }
        } else {
            std::printf("[engine] SimpleSkin not found or not skinned, using fallback\n"); std::fflush(stdout);
            if (!gltf.ok) { std::printf("[engine] gltf error: %s\n", gltf.error.c_str()); std::fflush(stdout); }
        }
    }

    Input::init(window_);
    jobs_.init(std::max(2u, std::thread::hardware_concurrency() - 1));

    // ENGINE_FPS_LOG=1 enables once-per-second timing output on stderr.
    const char* fpsLog = std::getenv("ENGINE_FPS_LOG");
    debugTiming_ = fpsLog && fpsLog[0] == '1';
}

Engine::~Engine() {
    jobs_.shutdown();
    gltfMeshes_.clear();
    delete cubeMesh_;
    delete triangleMesh_;
    delete scene_;
    delete renderer_;
}

void Engine::run() {
    RenderSystem renderSystem(*renderer_);

    while (!window_.shouldClose()) {
        time_.beginFrame();
        window_.pollEvents();

        // Task 031: F1 toggles node editor; overlay recorded inside main pass.
        static bool wasF1 = false;
        bool f1 = Input::isKeyPressed(GLFW_KEY_F1);
        if (f1 && !wasF1) editorOpen_ = !editorOpen_;
        renderer_->editorBeginFrame();
        if (editorOpen_ && editor_) {
            editor_->draw(editorBaseSkeleton_, [&](std::shared_ptr<AnimGraph> g) {
                auto* arr = scene_->registry().tryGetComponentArray<AnimationComponent>();
                if (!arr) return;
                for (size_t i = 0; i < arr->size(); ++i) {
                    auto& comp = arr->get(arr->entityAt(i));
                    if (comp.machine && comp.machine->setStateGraph("Locomotion", g)) break;
                }
                std::printf("[editor] applied graph to Locomotion (%zu nodes)\n", g->ownedNodes.size());
                std::fflush(stdout);
            });
        }
        renderer_->editorEndFrame();

        update(time_.deltaTime());
        world_->update(scene_->camera().position);

        renderSystem.beginFrame(scene_->camera(), scene_->light());
        renderSystem.update(scene_->registry());
        renderSystem.endFrame();

        if (debugTiming_ && time_.fps() > 0.0) {
            static double lastLoggedFps = -1.0;
            if (time_.fps() != lastLoggedFps) {
                lastLoggedFps = time_.fps();
                std::fprintf(stderr, "[engine] fps=%.1f dt=%.4fs\n", time_.fps(), time_.deltaTime());
            }
        }
    }
}


void Engine::update(double deltaTime) {
    // Simulation step: camera controller first, then scene systems (physics/AI later).
    controller_.update(scene_->camera(), static_cast<float>(deltaTime));

    // Animation blending + state machine (Task 030) — auto demo fallback ensures
    // transitions visible without manual input during headless capture.
    float dt = static_cast<float>(deltaTime);
    auto* skelArray = scene_->registry().tryGetComponentArray<SkeletonComponent>();
    auto* animArray = scene_->registry().tryGetComponentArray<AnimationComponent>();
    bool uploaded = false;

    static float autoTimer = 0.0f;
    autoTimer += dt;
    float autoSpeed = 0.0f;
    if (autoTimer < 2.0f) autoSpeed = 0.0f;          // Idle
    else if (autoTimer < 4.0f) autoSpeed = 1.0f;    // Walk
    else if (autoTimer < 6.0f) autoSpeed = 2.5f;    // Run
    else {
        autoTimer = 0.0f;
        autoSpeed = 0.0f;
    }

    float inputSpeed = 0.0f;
    bool hasInput = false;
    if (Input::isKeyPressed(GLFW_KEY_W)) {
        hasInput = true;
        inputSpeed = Input::isKeyPressed(GLFW_KEY_LEFT_SHIFT) ? 2.5f : 1.0f;
    } else if (Input::isKeyPressed(GLFW_KEY_S)) {
        hasInput = true;
        inputSpeed = 0.3f;
    }
    float desiredSpeed = hasInput ? inputSpeed : autoSpeed;

    // Jump is edge-triggered so holding SPACE doesn't re-fire.
    static bool prevSpace = false;
    bool space = Input::isKeyPressed(GLFW_KEY_SPACE);
    bool jumpPressed = space && !prevSpace;
    prevSpace = space;

    if (skelArray && animArray) {
        for (size_t i = 0; i < skelArray->size(); ++i) {
            Entity e = skelArray->entityAt(i);
            if (!animArray->has(e)) continue;
            auto& skelC = skelArray->get(e);
            auto& animC = animArray->get(e);
            if (animC.animations.empty() || !animC.playing) continue;
            // Task 030: state machine -> active graph -> pose. Engine only feeds params.
            if (!animC.machine) continue;
            animC.machine->params().speed = desiredSpeed;
            animC.machine->params().jumpPressed = jumpPressed;
            animC.machine->evaluateInto(skelC.skeleton, dt);
            renderer_->updateJoints(skelC.skeleton.finalMatrices);
            uploaded = true;
            break;
        }
    }
    if (!uploaded) {
        if (skelArray && skelArray->size() > 0) {
            auto& skelC = skelArray->get(skelArray->entityAt(0));
            if (skelC.skeleton.finalMatrices.empty()) skelC.skeleton.resizePose();
            computeFinalMatrices(skelC.skeleton);
            renderer_->updateJoints(skelC.skeleton.finalMatrices);
        } else {
            std::vector<glm::mat4> identity(kMaxJoints, glm::mat4(1.0f));
            renderer_->updateJoints(identity);
        }
    }
}

} // namespace engine
