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

        // Streaming world: shared quad mesh for ground chunks, rotated to XZ plane.
    triangleMesh_ = new Mesh(mesh_primitives::quad());
    world_ = new World(scene_->registry(), triangleMesh_, 2, 16.0f);

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
}

} // namespace engine
