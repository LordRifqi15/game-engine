cmake_minimum_required(VERSION 3.24)

project(game_engine
    VERSION 0.1.0
    DESCRIPTION "Custom game engine (Vulkan + GLFW)"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Vulkan REQUIRED)
find_package(glfw3 3.3 REQUIRED CONFIG)
find_package(glm REQUIRED CONFIG)

add_executable(engine
    third_party/tinyobjloader/tiny_obj_loader.cc
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_glfw.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp
    src/main.cpp
    src/core/application.cpp
    src/core/camera_controller.cpp
    src/core/engine.cpp
    src/core/job_system.cpp
    src/core/time.cpp
    src/core/world.cpp
    src/core/mesh.cpp
    src/core/mesh_loader.cpp
    src/core/gltf_loader.cpp
    src/core/animation_system.cpp
    src/core/anim_graph.cpp
    src/core/anim_state_machine.cpp
    src/core/anim_editor.cpp
    src/core/anim_graph_asset.cpp
    src/core/anim_graph_ai.cpp
    src/core/gameplay_graph.cpp
    src/core/scene/Scene.cpp
    src/core/scene/SceneSerializer.cpp
    src/modules/ai/GameplayNodesAI.cpp
    src/modules/ai/BlackboardNodes.cpp
    src/modules/interaction/InteractionNodes.cpp
    src/modules/interaction/InteractionSystem.cpp
    src/modules/navigation/NavigationNodes.cpp
    src/modules/navigation/NavGrid.cpp
    src/modules/navigation/Pathfinding.cpp
    src/modules/physics/PhysicsSystem.cpp
    src/editor/anim_graph_editor.cpp
    src/editor/SceneEditor.cpp
    src/platform/window.cpp
    src/platform/input.cpp
    src/renderer/renderer.cpp
    src/renderer/deferred/GBuffer.cpp
    src/renderer/deferred/DeferredPipeline.cpp
    src/renderer/lighting/ClusteredLighting.cpp
    src/renderer/culling/HiZPyramid.cpp
    src/renderer/culling/GPUCullingSystem.cpp
    src/renderer/scheduler/TimelineSync.cpp
    src/renderer/scheduler/FrameScheduler.cpp
    src/renderer/material/BindlessDescriptorManager.cpp
    src/renderer/material/TextureRegistry.cpp
    src/renderer/meshlet/MeshletBuilder.cpp
    src/renderer/meshlet/MeshletPipeline.cpp
    src/renderer/streaming/PageSlotPool.cpp
    src/renderer/streaming/MeshStreamingManager.cpp
    src/renderer/vulkan/VkFeatureManager.cpp
    src/renderer/graph/RenderGraph.cpp
    src/renderer/graph/RenderGraphBuilder.cpp
    src/renderer/graph/RenderGraphResources.cpp
    src/renderer/graph/ResourceLifetime.cpp
    src/renderer/graph/TransientResourcePool.cpp
    src/renderer/vulkan/VkBarrierHelper.cpp
    src/renderer/vulkan/VkDeviceMemoryHelper.cpp
    src/renderer/vulkan/vulkan_instance.cpp
    src/renderer/vulkan/vk_device.cpp
    src/renderer/vulkan/vk_swapchain.cpp
    src/renderer/vulkan/vk_pipeline.cpp
    src/renderer/vulkan/vk_command_buffer.cpp
    src/renderer/vulkan/gpu_occlusion.cpp
    src/renderer/vulkan/texture_cache.cpp
    src/renderer/vulkan/environment.cpp
    src/renderer/vulkan/shadow_pass.cpp
    src/renderer/vulkan/vk_renderer.cpp
)

find_program(GLSLC_EXECUTABLE glslc REQUIRED)

set(SHADER_SRC
    shaders/basic.vert
    shaders/basic.frag
    shaders/shadow.vert
    shaders/shadow.frag
    shaders/prepass.frag
    shaders/skybox.vert
    shaders/skybox.frag
    shaders/cull.comp
    shaders/hiz.comp
    shaders/cluster_cull.comp
    shaders/hiz_build.comp
    shaders/instance_cull.comp
    shaders/gbuffer.vert
    shaders/gbuffer.frag
    shaders/gbuffer_bindless.vert
    shaders/gbuffer_bindless.frag
    shaders/meshlet_cull.comp
    shaders/gbuffer_meshlet.vert
    shaders/meshlet_cull_stream.comp
    shaders/deferred_lighting.vert
    shaders/deferred_lighting.frag
)
set(SHADER_OUT
    ${CMAKE_BINARY_DIR}/shaders/basic.vert.spv
    ${CMAKE_BINARY_DIR}/shaders/basic.frag.spv
    ${CMAKE_BINARY_DIR}/shaders/shadow.vert.spv
    ${CMAKE_BINARY_DIR}/shaders/shadow.frag.spv
    ${CMAKE_BINARY_DIR}/shaders/prepass.frag.spv
    ${CMAKE_BINARY_DIR}/shaders/skybox.vert.spv
    ${CMAKE_BINARY_DIR}/shaders/skybox.frag.spv
    ${CMAKE_BINARY_DIR}/shaders/cull.comp.spv
    ${CMAKE_BINARY_DIR}/shaders/hiz.comp.spv
    ${CMAKE_BINARY_DIR}/shaders/cluster_cull.comp.spv
    ${CMAKE_BINARY_DIR}/shaders/hiz_build.comp.spv
    ${CMAKE_BINARY_DIR}/shaders/instance_cull.comp.spv
    ${CMAKE_BINARY_DIR}/shaders/gbuffer.vert.spv
    ${CMAKE_BINARY_DIR}/shaders/gbuffer.frag.spv
    ${CMAKE_BINARY_DIR}/shaders/gbuffer_bindless.vert.spv
    ${CMAKE_BINARY_DIR}/shaders/gbuffer_bindless.frag.spv
    ${CMAKE_BINARY_DIR}/shaders/meshlet_cull.comp.spv
    ${CMAKE_BINARY_DIR}/shaders/gbuffer_meshlet.vert.spv
    ${CMAKE_BINARY_DIR}/shaders/meshlet_cull_stream.comp.spv
    ${CMAKE_BINARY_DIR}/shaders/deferred_lighting.vert.spv
    ${CMAKE_BINARY_DIR}/shaders/deferred_lighting.frag.spv
)

add_custom_command(
    OUTPUT ${SHADER_OUT}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
    COMMAND ${GLSLC_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/basic.vert -o ${CMAKE_BINARY_DIR}/shaders/basic.vert.spv
    COMMAND ${GLSLC_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/basic.frag -o ${CMAKE_BINARY_DIR}/shaders/basic.frag.spv
    COMMAND ${GLSLC_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/shadow.vert -o ${CMAKE_BINARY_DIR}/shaders/shadow.vert.spv
    COMMAND ${GLSLC_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/shadow.frag -o ${CMAKE_BINARY_DIR}/shaders/shadow.frag.spv
    COMMAND ${GLSLC_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/skybox.vert -o ${CMAKE_BINARY_DIR}/shaders/skybox.vert.spv
    COMMAND ${GLSLC_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/skybox.frag -o ${CMAKE_BINARY_DIR}/shaders/skybox.frag.spv
    COMMAND ${GLSLC_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/cull.comp -o ${CMAKE_BINARY_DIR}/shaders/cull.comp.spv
    COMMAND ${GLSLC_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/cluster_cull.comp -o ${CMAKE_BINARY_DIR}/shaders/cluster_cull.comp.spv
    COMMAND ${GLSLC_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/hiz_build.comp -o ${CMAKE_BINARY_DIR}/shaders/hiz_build.comp.spv
    COMMAND ${GLSLC_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/instance_cull.comp -o ${CMAKE_BINARY_DIR}/shaders/instance_cull.comp.spv
    COMMAND ${GLSLC_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/prepass.frag -o ${CMAKE_BINARY_DIR}/shaders/prepass.frag.spv
    COMMAND ${GLSLC_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/hiz.comp -o ${CMAKE_BINARY_DIR}/shaders/hiz.comp.spv
    COMMAND ${GLSLC_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gbuffer.vert -o ${CMAKE_BINARY_DIR}/shaders/gbuffer.vert.spv
    COMMAND ${GLSLC_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gbuffer.frag -o ${CMAKE_BINARY_DIR}/shaders/gbuffer.frag.spv
    COMMAND ${GLSLC_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gbuffer_bindless.vert -o ${CMAKE_BINARY_DIR}/shaders/gbuffer_bindless.vert.spv
    COMMAND ${GLSLC_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gbuffer_bindless.frag -o ${CMAKE_BINARY_DIR}/shaders/gbuffer_bindless.frag.spv
    COMMAND ${GLSLC_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/meshlet_cull.comp -o ${CMAKE_BINARY_DIR}/shaders/meshlet_cull.comp.spv
    COMMAND ${GLSLC_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gbuffer_meshlet.vert -o ${CMAKE_BINARY_DIR}/shaders/gbuffer_meshlet.vert.spv
    COMMAND ${GLSLC_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/meshlet_cull_stream.comp -o ${CMAKE_BINARY_DIR}/shaders/meshlet_cull_stream.comp.spv
    COMMAND ${GLSLC_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/deferred_lighting.vert -o ${CMAKE_BINARY_DIR}/shaders/deferred_lighting.vert.spv
    COMMAND ${GLSLC_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/deferred_lighting.frag -o ${CMAKE_BINARY_DIR}/shaders/deferred_lighting.frag.spv
    DEPENDS ${SHADER_SRC}
    COMMENT "Compiling shaders"
)
add_custom_target(shaders ALL DEPENDS ${SHADER_OUT})
target_include_directories(engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tinyobjloader
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tinygltf
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends
)

target_link_libraries(engine PRIVATE
    Vulkan::Vulkan
    glfw
    glm::glm
)
