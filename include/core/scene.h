#pragma once

#include "core/camera.h"
#include "core/entity.h"
#include "core/light.h"
#include "core/material_component.h"
#include "core/mesh_component.h"
#include "core/registry.h"
#include "core/transform_component.h"

namespace engine {

class Renderer;

// Thin ECS wrapper: owns registry + camera + light.
// Entity creation returns IDs; data lives in components.
class Scene {
public:
    Entity createEntity() { return registry_.createEntity(); }

    // Convenience: entity with all three render components.
    Entity createRenderable(Mesh* mesh, const TransformComponent& transform,
                            const Material& material) {
        Entity e = registry_.createEntity();
        registry_.addComponent<TransformComponent>(e, transform);
        MeshComponent mc;
        mc.mesh = mesh;
        registry_.addComponent<MeshComponent>(e, mc);
        MaterialComponent mat;
        mat.material = material;
        registry_.addComponent<MaterialComponent>(e, mat);
        return e;
    }

    Registry& registry() { return registry_; }
    Camera& camera() { return camera_; }
    DirectionalLight& light() { return light_; }

private:
    Registry registry_;
    Camera camera_;
    DirectionalLight light_;
};

} // namespace engine
