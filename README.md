# Game Engine

Experimental C++17 game engine built around Vulkan, GLFW, a custom ECS, visual scripting, and editor tooling.

The project is an active prototype. Task 053 cut the newer render-graph renderer (`SceneRenderer`) over to the default live path: `ENGINE_RENDERER` unset runs modern, `ENGINE_RENDERER=legacy` runs the legacy Vulkan path. Modern renders statics through meshlets + Hi-Z, skinned through the classic path, with shadow cascades, deferred lighting, skybox, and tonemap. Headless suites green; see Tests.

## Highlights

- C++17 codebase with CMake build.
- Vulkan renderer with GLFW window and surface creation.
- Custom entity-component registry and scene serialization.
- Camera, input, time, physics, navigation, and interaction systems.
- Skeletal animation, animation state machines, and animation graph editing.
- Gameplay graphs with per-entity blackboard and AI behavior.
- ImGui-based scene, animation, and gameplay editors.
- GLTF, OBJ, mesh, material, and texture loading paths.
- GPU-oriented renderer modules for:
  - deferred G-buffer rendering;
  - clustered lighting;
  - GPU instance and meshlet culling;
  - hierarchical-Z occlusion;
  - bindless materials;
  - meshlet construction;
  - virtual geometry and mesh streaming;
  - transient render-graph resources and aliasing.
- Standalone test sources covering core gameplay and renderer subsystems.

## Current Runtime Flow

The executable starts through this path (modern default):

```text
main.cpp
  -> engine::Application
  -> engine::Engine
  -> engine::Engine::run()
  -> engine::RenderSystem (extracts GPUScene: draws, materials, lights, joints)
  -> Engine::Renderer + SceneRenderer (render-graph path)
```

Each frame the modern path runs roughly this sequence:

1. Poll window events.
2. Update camera, gameplay, physics, navigation, and animation.
3. Extract `GPUScene` from the ECS registry; bake statics to meshlets on change.
4. Shadow cascades, cluster light culling, Hi-Z pyramid build.
5. Meshlet frustum/cone/Hi-Z cull + index compaction (single indirect draw).
6. G-buffer (meshlet statics + classic skinned), deferred lighting, skybox.
7. Tonemap to swapchain, editor overlay, present.

`ENGINE_RENDERER=legacy` selects the legacy `engine::Renderer` to `VkRenderer` path instead.

## Renderer Paths

### Legacy live path

The live `Engine` runtime uses:

```text
include/renderer/renderer.h
src/renderer/renderer.cpp
include/renderer/vulkan/vk_renderer.h
src/renderer/vulkan/vk_renderer.cpp
```

`RenderSystem` submits scene-facing draw calls through this API. The Vulkan backend owns the device-facing renderer, swapchain, pipelines, command buffers, texture cache, and editor overlay.

### New render-graph path

The repository also contains a newer renderer API:

```text
include/renderer/Renderer.hpp
src/renderer/Renderer.cpp
include/renderer/FrameContext.hpp
src/renderer/FrameContext.cpp
```

Its declared frame graph contains these stages:

```text
Shadow
Cluster light culling
Hi-Z build
Meshlet culling and compaction
G-buffer
Deferred lighting
Forward skybox and transparents
Post-processing and tonemapping
Editor overlay
Presentation
```
This path is connected to:

- `RenderGraph` for pass dependencies and resource lifetimes;
- `TransientResourcePool` for transient allocation and aliasing;
- `FrameScheduler` for graphics and async-compute scheduling;
- `FrameContext` for per-frame camera, buffers, swapchain, and scene data.

It is the default live path used by `engine::Engine` (falls back to legacy only if `SceneRenderer` init fails). Verified: parity scene renders 30 draws / 29 meshlets / 3 lights with 0 validation errors; see `assets/scenes/renderer_parity.scene.json`.

Known gaps (Task 053 follow-ups): skinned player not yet visible in captures (classic path unverified), no visible cast shadows (shadow pass executes; receive/coverage unverified), legacy backend shows the editor overlay so no automated pixel parity exists.

## Requirements

- CMake 3.24 or newer.
- C++17 compiler.
- Vulkan SDK and loader.
- GLFW 3.3 or newer with CMake package configuration.
- GLM with CMake package configuration.
- `glslc` available on `PATH` for shader compilation.
- A Vulkan-capable GPU and desktop session for running the application.

The repository vendors several dependencies under `third_party/`, including Dear ImGui, tinygltf, tinyobjloader, and stb.

## Build

From the project root:

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
```

The build creates:

```text
build/engine
build/shaders/*.spv
```

## Run

```bash
./build/engine
```

The application starts a `1280x720` window titled `Game Engine`.

The engine searches several relative locations for scene files, including:

```text
assets/scenes/default.scene.json
assets/scenes/demo_world.scene.json
build/assets/scenes/default.scene.json
../assets/scenes/default.scene.json
```

If no scene is found, it creates a fallback scene. The fallback skeletal-model loader currently contains a machine-specific absolute path for the `SimpleSkin` asset, so a portable checkout may use the fallback behavior until that path is configured.

## Editor Controls

- `F1` toggles the animation graph editor.
- `F2` toggles the gameplay graph editor.

The scene editor and inspector are rendered through the ImGui overlay when the application is running.

## Repository Layout

```text
.
├── assets/                 Example scenes, graphs, models, and textures
├── include/                Public C++ headers
├── src/
│   ├── core/               Engine, ECS, scene, animation, and gameplay code
│   ├── editor/             ImGui editor implementations
│   ├── modules/            AI, interaction, navigation, and physics modules
│   ├── platform/           Window, input, and platform code
│   └── renderer/           Vulkan, render graph, scheduling, and GPU systems
├── shaders/                GLSL compute, vertex, and fragment shaders
├── tests/                  Standalone subsystem test sources
├── third_party/            Vendored dependencies
└── CMakeLists.txt          Build and shader compilation configuration
```

## Tests

Test sources are located in `tests/` and cover areas including:

- animation graphs and state machines;
- gameplay and per-entity AI graphs;
- physics, navigation, and interaction;
- scene serialization;
- render-graph compilation and aliasing;
- clustered lighting and deferred rendering;
- GPU culling and Hi-Z logic;
- meshlets and mesh streaming;
- bindless materials;
- frame scheduling and frame orchestration;
- visual pipeline contract (`tests/runtime_visual_pipeline_test.cpp`): Hi-Z tail-mip writability, mip-in-chain selection, frustum keep/cull.

Each test file carries a `// Build:` one-liner (standalone `g++`, no CTest registration). Green: visual-pipeline, occlusion-hiz, meshlet, render-graph + aliasing, deferred, clustered-lighting, bindless-material. `runtime_renderer_test` / `frame_pipeline_test` need a full-engine link (pre-existing staleness); covered by the CMake `engine` build plus live capture runs instead.

## Task 053 Notes (Hi-Z Tail-Mip Fix)

The Hi-Z build dispatch extent must clamp to a minimum of 1 per mip (`HiZPyramid::mipExtent`). Raw `extent >> m` underflows to 0 for small non-square tails (720p: `360>>9 = 0`), the build shader early-outs, the unwritten tail reads 0.0, and every meshlet whose lookup lands there is over-culled (nearest depth > 0). Symptom was a missing cube with `idx` stuck at ground-only counts; fixed in `SceneRenderer::recordHiZBuild`.

## Development Notes

This project is intentionally evolving toward a modular, data-oriented renderer and editor. When changing renderer code, verify both paths separately:

1. the live legacy `engine::Renderer` to `VkRenderer` path;
2. the newer `Engine::Renderer` render-graph and scheduler path.

They currently do not have identical initialization, resource, command-recording, or presentation behavior.

## License

No license file is currently included. Treat the project as unlicensed unless a license is added by the author.
