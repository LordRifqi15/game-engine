#include "core/engine.h"

#include "core/bounds_component.h"
#include "core/mesh.h"
#include "core/gltf_loader.h"
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
    scene_ = new Scene();

    // Test world (ECS): shared meshes, entities = IDs, data = components.
    triangleMesh_ = new Mesh(mesh_primitives::triangle());
    for (int x = -5; x <= 5; ++x) {
        for (int z = -5; z <= 5; ++z) {
            TransformComponent t;
            t.position = {static_cast<float>(x), 0.0f, static_cast<float>(z)};
            Material m;
            m.baseColor = {(static_cast<float>(x) + 5.0f) / 10.0f,
                           0.5f,
                           (static_cast<float>(z) + 5.0f) / 10.0f,
                           1.0f};
            scene_->createRenderable(triangleMesh_, t, m);
        }
    }

    // GLTF model: one entity per primitive; materials + textures from file.
    auto gltfPrims = loadGLTF("assets/models/cube.gltf", &renderer_->textureCache());
    const glm::vec3 modelSpots[] = {{-2.0f, 1.5f, -2.0f}, {2.0f, 1.5f, -1.0f}, {0.0f, 1.5f, -4.0f}};
    // Reserve first: pointers into gltfMeshes_ must stay stable.
    gltfMeshes_.reserve(sizeof(modelSpots) / sizeof(modelSpots[0]) * gltfPrims.size());
    for (const auto& spot : modelSpots) {
        for (const auto& prim : gltfPrims) {
            gltfMeshes_.push_back(prim.mesh);
            TransformComponent t;
            t.position = spot;
            Entity e = scene_->createRenderable(&gltfMeshes_.back(), t, prim.material);
            BoundsComponent b;
            b.radius = 1.8f;
            scene_->registry().addComponent<BoundsComponent>(e, b);
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
    RenderSystem renderSystem(*renderer_, jobs_);
    renderSystem.setCamera(scene_->camera());

    while (!window_.shouldClose()) {
        time_.beginFrame();
        window_.pollEvents();
        update(time_.deltaTime());
        renderSystem.setCamera(scene_->camera());

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
}

} // namespace engine
