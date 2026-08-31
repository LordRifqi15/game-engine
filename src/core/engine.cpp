#include "core/engine.h"

#include "core/anim_graph.h"
#include "core/anim_graph_asset.h"
#include "core/anim_state_machine.h"
#include "core/animation_system.h"
#include "core/bounds_component.h"
#include "core/gameplay_component.h"
#include "core/gameplay_graph.h"
#include "ecs/components/PhysicsComponent.hpp"
#include "ecs/components/ColliderComponent.hpp"
#include "modules/ai/GameplayNodesAI.hpp"
#include "modules/ai/GraphContext.hpp"
#include "imgui.h"
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
                    // Task 031: editor mirrors locomotion graph for visual editing (once)
                    if (!editor_) {
                        editorGraph_ = makeLocomotionEditorGraph(animC.animations);
                        editorBaseSkeleton_ = skelC.skeleton;
                        editor_ = std::make_unique<AnimGraphEditor>(editorGraph_);
                        std::printf("[editor] graph editor ready: %zu nodes, output %d\n", editorGraph_.nodes.size(), editorGraph_.outputNode);
                        std::fflush(stdout);
                        // Task 035: gameplay editor (per-entity, F2)
                        if (!gameplayEditor_) {
                            gameplayEditorGraph_ = makeGameplayEditorGraph();
                            gameplayEditor_ = std::make_unique<AnimGraphEditor>(gameplayEditorGraph_);
                            gameplayEditorTarget_ = e;
                            std::printf("[gameplay] editor ready: %zu nodes\n", gameplayEditorGraph_.nodes.size());
                            std::fflush(stdout);
                        }
                    }
                    // Task 035: per-entity gameplay graph (clone template per entity)
                    {
                        GameplayComponent gc;
                        gc.graph = GameplayGraph::makeMinimal(&animC.machine->params());
                        scene_->registry().addComponent<GameplayComponent>(e, std::move(gc));
                        // Task 037: physics + collider for player
                        PhysicsComponent pc;
                        pc.velocity = glm::vec3(0.0f);
                        pc.mass = 1.0f;
                        pc.linearDamping = 10.0f;
                        pc.useGravity = true;
                        pc.isGrounded = true;
                        ::Engine::ColliderComponent col;
                        col.type = ::Engine::ColliderType::Sphere;
                        col.radius = 0.5f;
                        col.halfExtents = glm::vec3(0.5f, 1.0f, 0.5f);
                        col.centerOffset = glm::vec3(0.0f, 0.5f, 0.0f);
                        scene_->registry().addComponent<::Engine::ColliderComponent>(e, col);
                    }
                    if (editor_ && editorGraph_.nodes.size() > 0) {
                        const std::vector<std::string> candidates = {
                            "assets/animations/locomotion.graph.json",
                            "build/assets/animations/locomotion.graph.json",
                            "../assets/animations/locomotion.graph.json"
                        };
                        EditorGraph loaded;
                        bool loadedOk = false;
                        std::string loadedPath;
                        for (auto& p : candidates) {
                            if (loadGraph(loaded, p)) { loadedOk = true; loadedPath = p; break; }
                        }
                        if (loadedOk) {
                            auto res = buildRuntimeGraph(loaded, skelC.skeleton, animC.animations);
                            if (!res.graph) res = buildRuntimeGraph(loaded, skelC.skeleton);
                            if (res.graph) {
                                // Only override first entity's locomotion when loading persisted graph
                                static bool firstLoad = true;
                                if (firstLoad) {
                                    editorGraph_ = loaded;
                                    editor_ = std::make_unique<AnimGraphEditor>(editorGraph_);
                                    editorBaseSkeleton_ = skelC.skeleton;
                                    if (animC.machine) animC.machine->setStateGraph("Locomotion", res.graph);
                                    std::printf("[asset] loaded %s (%zu nodes) and applied to Locomotion\n", loadedPath.c_str(), loaded.nodes.size());
                                    std::fflush(stdout);
                                    firstLoad = false;
                                }
                            } else {
                                std::printf("[asset] build failed for %s: %s\n", loadedPath.c_str(), res.error.c_str());
                                std::fflush(stdout);
                            }
                        } else {
                            static bool once = true;
                            if (once) { std::printf("[asset] no persisted graph found, using default\n"); std::fflush(stdout); once = false; }
                        }
                    }
                    scene_->registry().addComponent<AnimationComponent>(e, std::move(animC));
                    if (playerEntity_ == kInvalidEntity) {
                        playerEntity_ = e;
                        gameplayEditorTarget_ = e;
                    }
                }
            }
            // Task 036: spawn NPC(s) that autonomously chase player
            if (playerEntity_ != kInvalidEntity && gltf.ok && !gltf.skeletons.empty() && !gltf.animations.empty()) {
                // Reuse first mesh/material for NPCs (SimpleSkin has 1 prim)
                for (int npcIdx = 0; npcIdx < 2; ++npcIdx) {
                    Entity npc = scene_->registry().createEntity();
                    TransformComponent tc;
                    tc.position = glm::vec3(5.0f + npcIdx * 3.0f, 0.0f, 2.0f + npcIdx * 1.5f);
                    tc.scale = glm::vec3(1.0f);
                    scene_->registry().addComponent<TransformComponent>(npc, tc);
                    // Task 037: physics + collider for NPC
                    {
                        PhysicsComponent pc;
                        pc.velocity = glm::vec3(0.0f);
                        pc.mass = 1.0f;
                        pc.linearDamping = 8.0f;
                        pc.useGravity = true;
                        pc.isGrounded = true;
                        scene_->registry().addComponent<PhysicsComponent>(npc, pc);
                        ::Engine::ColliderComponent col;
                        col.type = ::Engine::ColliderType::Sphere;
                        col.radius = 0.5f;
                        col.halfExtents = glm::vec3(0.5f, 1.0f, 0.5f);
                        col.centerOffset = glm::vec3(0.0f, 0.5f, 0.0f);
                        scene_->registry().addComponent<::Engine::ColliderComponent>(npc, col);
                    }
                    if (!gltfMeshes_.empty()) {
                        MeshComponent mc; mc.mesh = &gltfMeshes_.front();
                        scene_->registry().addComponent<MeshComponent>(npc, mc);
                    }
                    MaterialComponent matC2;
                    matC2.material = gltf.primitives.empty() ? Material{} : gltf.primitives.front().material;
                    scene_->registry().addComponent<MaterialComponent>(npc, matC2);
                    SkeletonComponent skelC2;
                    skelC2.skeleton = gltf.skeletons[0];
                    scene_->registry().addComponent<SkeletonComponent>(npc, skelC2);
                    AnimationComponent animC2;
                    animC2.animations = gltf.animations;
                    if (animC2.animations.size() == 1) {
                        auto copy = animC2.animations[0];
                        copy.name = copy.name.empty() ? "Copy" : copy.name + "_Copy";
                        animC2.animations.push_back(std::move(copy));
                    }
                    animC2.speed = 1.0f;
                    animC2.playing = true;
                    animC2.loop = true;
                    float jd = animC2.animations.size() > 1 && animC2.animations[1].duration > 0.0f ? animC2.animations[1].duration : 1.0f;
                    animC2.machine = makeDefaultStateMachine(animC2.animations, skelC2.skeleton, jd);
                    GameplayComponent gc;
                    gc.graph = GameplayGraph::makeNPCChase(&animC2.machine->params(), 6.0f, 2.2f);
                    scene_->registry().addComponent<GameplayComponent>(npc, std::move(gc));
                    scene_->registry().addComponent<AnimationComponent>(npc, std::move(animC2));
                    std::printf("[npc] spawned NPC %u at (%.1f,0,%.1f) chasing player %u\n", npc, tc.position.x, tc.position.z, playerEntity_);
                    std::fflush(stdout);
                }
            }
        } else {
            std::printf("[engine] SimpleSkin not found or not skinned, using fallback\n"); std::fflush(stdout);
            if (!gltf.ok) { std::printf("[engine] gltf error: %s\n", gltf.error.c_str()); std::fflush(stdout); }
        }
    }

    Input::init(window_);

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
        window_.pollEvents();
        time_.beginFrame();
        // Task 031: F1 toggles node editor; overlay recorded inside main pass.
        static bool wasF1 = false;
        static bool wasF2 = false;
        bool f1 = Input::isKeyPressed(GLFW_KEY_F1);
        bool f2 = Input::isKeyPressed(GLFW_KEY_F2);
        if (f1 && !wasF1) editorOpen_ = !editorOpen_;
        if (f2 && !wasF2) gameplayEditorOpen_ = !gameplayEditorOpen_;
        wasF1 = f1; wasF2 = f2;
        renderer_->editorBeginFrame();
        if (editorOpen_ && editor_) {
            // Task 033: editor resolves clip indices against live animations.
            auto* animArr = scene_->registry().tryGetComponentArray<AnimationComponent>();
            editor_->setAnimations(animArr && animArr->size() ? &animArr->get(animArr->entityAt(0)).animations : nullptr);
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
        if (gameplayEditorOpen_ && gameplayEditor_) {
            auto* animArr = scene_->registry().tryGetComponentArray<AnimationComponent>();
            auto* gameplayArr = scene_->registry().tryGetComponentArray<GameplayComponent>();
            if (gameplayArr && animArr && animArr->size()) {
                if (gameplayEditorTarget_ == kInvalidEntity || !gameplayArr->has(gameplayEditorTarget_)) {
                    for (size_t i=0;i<animArr->size();++i) {
                        Entity e = animArr->entityAt(i);
                        if (gameplayArr->has(e)) { gameplayEditorTarget_ = e; break; }
                    }
                    if (gameplayEditorTarget_ == kInvalidEntity) gameplayEditorTarget_ = animArr->entityAt(0);
                }
                ImGui::Begin("Gameplay Editor Target");
                for (size_t i=0;i<animArr->size();++i) {
                    Entity e = animArr->entityAt(i);
                    if (!gameplayArr->has(e)) continue;
                    bool sel = (e == gameplayEditorTarget_);
                    char buf[32]; std::snprintf(buf, sizeof(buf), "Entity %u", e);
                    if (ImGui::Selectable(buf, sel)) gameplayEditorTarget_ = e;
                }
                ImGui::End();
            }
            gameplayEditor_->draw(editorBaseSkeleton_, [&](std::shared_ptr<AnimGraph> g) {
                (void)g;
            });
            if (ImGui::Begin("Gameplay Graph Editor")) {
                if (ImGui::Button("Apply Gameplay to Selected Entity")) {
                    auto* gArr = scene_->registry().tryGetComponentArray<GameplayComponent>();
                    if (gArr && gArr->has(gameplayEditorTarget_)) {
                        auto& gc = gArr->get(gameplayEditorTarget_);
                        auto* animArr2 = scene_->registry().tryGetComponentArray<AnimationComponent>();
                        AnimParams* targetParams = nullptr;
                        if (animArr2 && animArr2->has(gameplayEditorTarget_)) {
                            auto& ac = animArr2->get(gameplayEditorTarget_);
                            if (ac.machine) targetParams = &ac.machine->params();
                        }
                        if (targetParams) {
                            auto newGraph = buildGameplayGraph(gameplayEditor_->getGraph(), targetParams);
                            if (newGraph) {
                                gc.graph = newGraph;
                                std::printf("[gameplay] applied editor graph to entity %u (%zu nodes)\n", gameplayEditorTarget_, newGraph->nodes.size());
                                std::fflush(stdout);
                            }
                        }
                    }
                }
                ImGui::End();
            }
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

    // Clamp dt for deterministic physics (avoid large spikes from window drag)
    float dt = static_cast<float>(deltaTime);
    if (dt > 0.1f) dt = 0.1f;
    if (dt <= 0.0f) return;

    auto* skelArray = scene_->registry().tryGetComponentArray<SkeletonComponent>();
    auto* animArray = scene_->registry().tryGetComponentArray<AnimationComponent>();
    auto* transformArray = scene_->registry().tryGetComponentArray<TransformComponent>();
    auto* gameplayArray = scene_->registry().tryGetComponentArray<GameplayComponent>();
    auto* physArray = scene_->registry().tryGetComponentArray<PhysicsComponent>();

    // Resolve player target position for NPCs (Task 036)
    glm::vec3 playerPos{0.0f};
    if (playerEntity_ != kInvalidEntity && transformArray && transformArray->has(playerEntity_)) {
        playerPos = transformArray->get(playerEntity_).position;
    } else if (transformArray && transformArray->size() > 0) {
        playerPos = transformArray->get(transformArray->entityAt(0)).position;
        if (playerEntity_ == kInvalidEntity && transformArray->size() > 0) {
            playerEntity_ = transformArray->entityAt(0);
        }
    }

    // 1. Gameplay & AI graphs (sets desired velocity / impulses) — per-entity
    if (skelArray && animArray) {
        for (size_t i = 0; i < skelArray->size(); ++i) {
            Entity e = skelArray->entityAt(i);
            if (!animArray->has(e)) continue;
            auto& animC = animArray->get(e);
            if (animC.animations.empty() || !animC.playing) continue;
            if (!animC.machine) continue;
            if (!gameplayArray) continue;
            auto* gComp = gameplayArray->tryGet(e);
            if (!gComp || !gComp->graph) continue;
            // NPCs (non-player) with Transform+Physics get spatial physics context
            if (e != playerEntity_ && transformArray && transformArray->has(e) && physArray && physArray->has(e)) {
                auto& tr = transformArray->get(e);
                auto& phys = physArray->get(e);
                GraphContext ctx{};
                ctx.selfEntity = static_cast<uint32_t>(e);
                ctx.targetEntity = static_cast<uint32_t>(playerEntity_);
                ctx.selfPosition = tr.position;
                ctx.targetPosition = playerPos;
                ctx.outPhysics = &phys;
                ctx.outSelfRotationEuler = &tr.rotation;
                ctx.dt = dt;
                // Legacy fallback also set for rotation
                ctx.outSelfPosition = &tr.position;
                gComp->graph->execute(ctx, animC.machine->params());
            } else if (e != playerEntity_ && transformArray && transformArray->has(e)) {
                // Fallback without physics (should not happen, but keep legacy)
                auto& tr = transformArray->get(e);
                GraphContext ctx{};
                ctx.selfEntity = static_cast<uint32_t>(e);
                ctx.targetEntity = static_cast<uint32_t>(playerEntity_);
                ctx.selfPosition = tr.position;
                ctx.targetPosition = playerPos;
                ctx.outSelfPosition = &tr.position;
                ctx.outSelfRotationEuler = &tr.rotation;
                ctx.dt = dt;
                gComp->graph->execute(ctx, animC.machine->params());
            } else {
                // Player or no transform: legacy Input-driven
                gComp->graph->execute(dt, animC.machine->params());
            }
        }
    }

    // 2. Physics simulation (integrates velocity, applies gravity, moves Transform)
    physicsSystem_.update(scene_->registry(), dt);

    // 3. Animation evaluation (reads actual horizontal velocity for locomotion blend) + upload
    bool uploaded = false;
    if (skelArray && animArray) {
        for (size_t i = 0; i < skelArray->size(); ++i) {
            Entity e = skelArray->entityAt(i);
            if (!animArray->has(e)) continue;
            auto& skelC = skelArray->get(e);
            auto& animC = animArray->get(e);
            if (animC.animations.empty() || !animC.playing) continue;
            if (!animC.machine) continue;
            // Decoupled pose: observe actual physics velocity
            if (physArray && physArray->has(e)) {
                auto& phys = physArray->get(e);
                float horizSpeed = glm::length(glm::vec2(phys.velocity.x, phys.velocity.z));
                animC.machine->params().speed = horizSpeed;
                animC.machine->params().isGrounded = phys.isGrounded;
            }
            animC.machine->evaluateInto(skelC.skeleton, dt);
            if (!uploaded) {
                renderer_->updateJoints(skelC.skeleton.finalMatrices);
                uploaded = true;
            }
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
