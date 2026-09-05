#pragma once
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

#include "renderer/lighting/LightTypes.hpp"

namespace engine {
class Mesh;
class Texture;
struct DirectionalLight;
} // namespace engine

namespace Engine {

// CPU-side frame snapshot. RenderGraph passes consume uploaded GPU buffers,
// never ECS. All pointers are stable for the frame (asset-owned); indices are
// renderer-assigned bindless IDs valid for the frame.
struct GPUSceneDraw {
    const ::engine::Mesh* mesh{nullptr};
    glm::mat4 model{1.0f};
    uint32_t materialID{0};
    bool skinned{false};
    uint32_t jointOffset{0};
    uint32_t jointCount{0};
};

struct GPUSceneMaterial {
    glm::vec4 baseColor{1.0f};
    float metallic{0.0f};
    float roughness{1.0f};
    const ::engine::Texture* albedoTexture{nullptr};
};

struct GPUSceneCamera {
    glm::mat4 view{1.0f};
    glm::mat4 proj{1.0f};
    glm::mat4 invViewProj{1.0f};
    glm::vec3 worldPosition{0.0f};
    float zNear{0.1f};
    float zFar{1000.0f};
    float fov{1.22f};
    float aspect{16.0f / 9.0f};
};

struct GPUSceneDirectional {
    glm::vec3 direction{0.0f, -1.0f, -1.0f};
    glm::vec3 color{1.0f};
    float intensity{1.0f};
    glm::mat4 shadowVP{1.0f};
};

struct GPUScene {
    GPUSceneCamera camera;
    GPUSceneDirectional directional;
    std::vector<GPUSceneDraw> draws;
    std::vector<GPUSceneMaterial> materials;
    std::vector<GPULight> lights; // point/spot only; directional travels separately
    std::vector<glm::mat4> joints;

    uint32_t staticDrawCount() const {
        uint32_t n = 0;
        for (auto& d : draws) n += d.skinned ? 0 : 1;
        return n;
    }
};

} // namespace Engine
