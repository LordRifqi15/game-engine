#pragma once

#include "core/instance_data.h"
#include "core/material_component.h"
#include "core/mesh_component.h"
#include "core/registry.h"
#include "core/transform_component.h"
#include "renderer/renderer.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
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

private:
    Renderer& renderer_;
};

} // namespace engine
