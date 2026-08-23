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
    src/main.cpp
    src/core/application.cpp
    src/core/camera_controller.cpp
    src/core/engine.cpp
    src/core/job_system.cpp
    src/core/time.cpp
    src/core/mesh.cpp
    src/core/mesh_loader.cpp
    src/core/gltf_loader.cpp
    src/platform/window.cpp
    src/platform/input.cpp
    src/renderer/renderer.cpp
    src/renderer/vulkan/vulkan_instance.cpp
    src/renderer/vulkan/vk_device.cpp
    src/renderer/vulkan/vk_swapchain.cpp
    src/renderer/vulkan/vk_pipeline.cpp
    src/renderer/vulkan/vk_command_buffer.cpp
    src/renderer/vulkan/texture_cache.cpp
    src/renderer/vulkan/vk_renderer.cpp
)

find_program(GLSLC_EXECUTABLE glslc REQUIRED)

set(SHADER_SRC
    shaders/basic.vert
    shaders/basic.frag
)
set(SHADER_OUT
    ${CMAKE_BINARY_DIR}/shaders/basic.vert.spv
    ${CMAKE_BINARY_DIR}/shaders/basic.frag.spv
)

add_custom_command(
    OUTPUT ${SHADER_OUT}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
    COMMAND ${GLSLC_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/basic.vert -o ${CMAKE_BINARY_DIR}/shaders/basic.vert.spv
    COMMAND ${GLSLC_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/basic.frag -o ${CMAKE_BINARY_DIR}/shaders/basic.frag.spv
    DEPENDS ${SHADER_SRC}
    COMMENT "Compiling shaders"
)
add_custom_target(shaders ALL DEPENDS ${SHADER_OUT})
add_dependencies(engine shaders)

target_include_directories(engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tinyobjloader
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tinygltf
)

target_link_libraries(engine PRIVATE
    Vulkan::Vulkan
    glfw
    glm::glm
)
