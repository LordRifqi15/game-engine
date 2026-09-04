#pragma once

#include "core/instance_data.h"
#include "core/material_component.h"
#include "core/mesh_component.h"
#include "core/registry.h"
#include "core/transform_component.h"
#include "renderer/renderer.h"
#include "renderer/FrameContext.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>

// Collects all renderable entities into a flat instance list.
// Visibility decisions happen on GPU via compute shader.
namespace engine {

class RenderSystem {
public:
    explicit RenderSystem(Renderer& renderer) : renderer_(renderer) {}

    void beginFrame(const Camera& camera, const DirectionalLight& light) {
        renderer_.beginFrame(camera, light);
    }

    void update(Registry& registry) {
        auto& transforms = registry.array<TransformComponent>();
        auto& meshes = registry.array<MeshComponent>();
        auto& materials = registry.array<MaterialComponent>();

        // No CPU culling: upload everything, GPU decides visibility.
        for (size_t i = 0; i < meshes.size(); ++i) {
            Entity e = meshes.entityAt(i);
            if (!transforms.has(e) || !materials.has(e)) continue;
            if (!meshes.get(e).mesh) continue;

            const TransformComponent& tc = transforms.get(e);
            InstanceData inst;
            glm::mat4 model = glm::mat4(1.0f);
            model = glm::translate(model, tc.position);
            model *= glm::mat4_cast(glm::quat(tc.rotation));
            model = glm::scale(model, tc.scale);
            inst.model = model;
            inst.color = materials.get(e).material.baseColor;
            inst.params = {materials.get(e).material.metallic,
                           materials.get(e).material.roughness, 0.0f, 0.0f};
            renderer_.drawMeshInstanced(*meshes.get(e).mesh, inst);
        }
    }

    void endFrame() { renderer_.endFrame(); }

    // Task 052: decoupled extraction. Flat scene snapshot for the render-graph
    // path; passes consume FrameContext, never the registry. Buffers stay null
    // until GPU upload arenas land; barrier paths already skip null handles.
    void extractGPUScene(Registry& registry, const Camera& camera, ::Engine::FrameContext& ctx) {
        Camera cam = camera;
        if (ctx.renderExtent.height > 0) {
            cam.aspect = static_cast<float>(ctx.renderExtent.width) /
                         static_cast<float>(ctx.renderExtent.height);
        }
        const glm::mat4 view = cam.viewMatrix();
        const glm::mat4 proj = cam.projectionMatrix();
        ctx.camera.viewMatrix = view;
        ctx.camera.projMatrix = proj;
        ctx.camera.invViewProj = glm::inverse(proj * view);
        ctx.camera.worldPosition = cam.position;
        ctx.camera.zNear = cam.nearPlane;
        ctx.camera.zFar = cam.farPlane;
        ctx.camera.fov = glm::radians(cam.fov);
        ctx.camera.aspectRatio = cam.aspect;

        uint32_t instances = 0;
        auto& transforms = registry.array<TransformComponent>();
        auto& meshes = registry.array<MeshComponent>();
        auto& materials = registry.array<MaterialComponent>();
        for (size_t i = 0; i < meshes.size(); ++i) {
            Entity e = meshes.entityAt(i);
            if (!transforms.has(e) || !materials.has(e)) continue;
            if (!meshes.get(e).mesh) continue;
            ++instances;
        }
        ctx.totalInstances = instances;
        ctx.totalMeshlets = 0;
        ctx.activeLightCount = 1; // scene directional light; no light SSBO yet
        ctx.globalVertexBuffer = VK_NULL_HANDLE;
        ctx.globalIndexBuffer = VK_NULL_HANDLE;
        ctx.globalInstanceBuffer = VK_NULL_HANDLE;
        ctx.globalMeshletBuffer = VK_NULL_HANDLE;
        ctx.globalMaterialBuffer = VK_NULL_HANDLE;
        ctx.globalLightBuffer = VK_NULL_HANDLE;
    }
private:
    Renderer& renderer_;
};

} // namespace engine
