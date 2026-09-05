#pragma once

#include "core/instance_data.h"
#include "core/material_component.h"
#include "core/mesh_component.h"
#include "core/registry.h"
#include "core/transform_component.h"
#include "renderer/renderer.h"
#include "renderer/FrameContext.hpp"
#include "renderer/GPUScene.hpp"
#include "renderer/scene/LightComponent.hpp"
#include "core/skeleton.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/quaternion.hpp>

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
    // path; passes consume GPUScene + uploaded GPU buffers, never the registry.
    void extractGPUScene(Registry& registry, const Camera& camera, const DirectionalLight& light,
                         ::Engine::GPUScene& scene, ::Engine::FrameContext& ctx) {
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
        scene.camera.view = view;
        scene.camera.proj = proj;
        scene.camera.invViewProj = glm::inverse(proj * view);
        scene.camera.worldPosition = cam.position;
        scene.camera.zNear = cam.nearPlane;
        scene.camera.zFar = cam.farPlane;
        scene.camera.fov = glm::radians(cam.fov);
        scene.camera.aspect = cam.aspect;
        scene.directional.direction = light.direction;
        scene.directional.color = light.color;
        scene.directional.intensity = 1.0f;

        scene.draws.clear();
        scene.materials.clear();
        scene.lights.clear();
        scene.joints.clear();
        auto& transforms = registry.array<TransformComponent>();
        auto& meshes = registry.array<MeshComponent>();
        auto& materials = registry.array<MaterialComponent>();
        for (size_t i = 0; i < meshes.size(); ++i) {
            Entity e = meshes.entityAt(i);
            if (!transforms.has(e) || !materials.has(e)) continue;
            const Mesh* mesh = meshes.get(e).mesh;
            if (!mesh || mesh->empty()) continue;
            const TransformComponent& tc = transforms.get(e);
            glm::mat4 model = glm::mat4(1.0f);
            model = glm::translate(model, tc.position);
            model *= glm::mat4_cast(glm::quat(tc.rotation));
            model = glm::scale(model, tc.scale);
            const Material& mat = materials.get(e).material;
            ::Engine::GPUSceneMaterial gm;
            gm.baseColor = mat.baseColor;
            gm.metallic = mat.metallic;
            gm.roughness = mat.roughness;
            gm.albedoTexture = mat.baseColorTexture;
            uint32_t matID = static_cast<uint32_t>(scene.materials.size());
            scene.materials.push_back(gm);
            ::Engine::GPUSceneDraw draw;
            draw.mesh = mesh;
            draw.model = model;
            draw.materialID = matID;
            if (auto* skel = registry.tryGetComponent<SkeletonComponent>(e)) {
                if (!skel->skeleton.finalMatrices.empty()) {
                    draw.skinned = true;
                    draw.jointOffset = static_cast<uint32_t>(scene.joints.size());
                    draw.jointCount = static_cast<uint32_t>(skel->skeleton.finalMatrices.size());
                    for (auto& m : skel->skeleton.finalMatrices) scene.joints.push_back(m);
                }
            }
            scene.draws.push_back(draw);
        }
        // Lights: directional components override the scene light; points feed clustering.
        if (auto* arr = registry.tryGetComponentArray<::Engine::DirectionalLightComponent>()) {
            for (size_t i = 0; i < arr->size(); ++i) {
                auto& lc = arr->get(arr->entityAt(i));
                scene.directional.direction = lc.direction;
                scene.directional.color = lc.color;
                scene.directional.intensity = lc.intensity;
            }
        }
        auto pushPoint = [&](const glm::vec3& pos, const glm::vec3& color, float intensity,
                             float radius) {
            ::Engine::GPULight l;
            l.positionRadius = glm::vec4(pos, radius > 0.0f ? radius : 10.0f);
            l.colorIntensity = glm::vec4(color, intensity);
            l.directionAngle = glm::vec4(0.0f, -1.0f, 0.0f, 0.0f);
            l.type = 1;
            l.castsShadows = 0;
            l.shadowMapIndex = 0;
            l.padding = 0.0f;
            scene.lights.push_back(l);
        };
        if (auto* arr = registry.tryGetComponentArray<::Engine::PointLightComponent>()) {
            for (size_t i = 0; i < arr->size(); ++i) {
                Entity e = arr->entityAt(i);
                auto& lc = arr->get(e);
                glm::vec3 pos = lc.position;
                if (transforms.has(e)) pos += transforms.get(e).position;
                pushPoint(pos, lc.color, lc.intensity, lc.radius);
            }
        }
        if (auto* arr = registry.tryGetComponentArray<::Engine::LightComponent>()) {
            for (size_t i = 0; i < arr->size(); ++i) {
                Entity e = arr->entityAt(i);
                auto& lc = arr->get(e);
                if (lc.type == ::Engine::LightComponent::Type::Point) {
                    glm::vec3 pos = lc.position;
                    if (transforms.has(e)) pos += transforms.get(e).position;
                    pushPoint(pos, lc.color, lc.intensity, lc.radius);
                } else {
                    scene.directional.direction = lc.direction;
                    scene.directional.color = lc.color;
                    scene.directional.intensity = lc.intensity;
                }
            }
        }
        ctx.totalInstances = static_cast<uint32_t>(scene.draws.size());
        ctx.totalMeshlets = 0;
        ctx.activeLightCount = static_cast<uint32_t>(scene.lights.size());
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
