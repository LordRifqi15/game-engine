#include "editor/SceneEditor.hpp"
#include "core/scene/SceneSerializer.hpp"
#include "ecs/components/TagComponent.hpp"
#include "core/transform_component.h"
#include "ecs/components/PhysicsComponent.hpp"
#include "ecs/components/ColliderComponent.hpp"
#include "ecs/components/BlackboardComponent.hpp"
#include "core/gameplay_component.h"

#include <imgui.h>
#include <cstring>

namespace Engine {

void SceneEditor::onImGuiRender(entt::registry& registry) {
    drawMenuBar(registry);
    drawHierarchy(registry);
    drawInspector(registry);
}

void SceneEditor::drawMenuBar(entt::registry& registry) {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("Scene")) {
            if (ImGui::MenuItem("Save Scene")) {
                SceneSerializer::serialize(currentScenePath_, registry);
            }
            if (ImGui::MenuItem("Load Scene")) {
                selectedEntity_ = entt::null;
                SceneSerializer::deserialize(currentScenePath_, registry);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (context_.state == SceneState::Edit) {
            if (ImGui::Button("▶ Play")) context_.state = SceneState::Play;
        } else {
            if (ImGui::Button("⏹ Stop")) context_.state = SceneState::Edit;
        }
        ImGui::EndMainMenuBar();
    }
}

void SceneEditor::drawHierarchy(entt::registry& registry) {
    ImGui::Begin("Scene Hierarchy");
    if (ImGui::Button("+ Add Entity")) {
        auto e = registry.create();
        registry.emplace<TagComponent>(e, TagComponent("New Entity"));
        registry.emplace<::engine::TransformComponent>(e, ::engine::TransformComponent{});
        selectedEntity_ = e;
    }
    ImGui::Separator();
    auto view = registry.view<TagComponent>();
    for (auto entity : view) {
        const auto& tag = view.get<TagComponent>(entity).tag;
        ImGuiTreeNodeFlags flags = ((selectedEntity_ == entity) ? ImGuiTreeNodeFlags_Selected : 0) | ImGuiTreeNodeFlags_OpenOnArrow;
        bool opened = ImGui::TreeNodeEx((void*)(uint64_t)(uint32_t)entity, flags, "%s", tag.c_str());
        if (ImGui::IsItemClicked()) {
            selectedEntity_ = entity;
        }
        if (opened) ImGui::TreePop();
    }
    ImGui::End();
}

void SceneEditor::drawInspector(entt::registry& registry) {
    ImGui::Begin("Inspector");
    if (selectedEntity_ == entt::null || !registry.valid(selectedEntity_)) {
        selectedEntity_ = entt::null;
        ImGui::Text("No entity selected.");
        ImGui::End();
        return;
    }
    if (registry.all_of<TagComponent>(selectedEntity_)) {
        auto& tag = registry.get<TagComponent>(selectedEntity_).tag;
        char buffer[256];
        std::strncpy(buffer, tag.c_str(), sizeof(buffer));
        buffer[sizeof(buffer)-1] = '\0';
        if (ImGui::InputText("Tag", buffer, sizeof(buffer))) {
            tag = std::string(buffer);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete Entity")) {
        registry.destroy(selectedEntity_);
        selectedEntity_ = entt::null;
        ImGui::End();
        return;
    }
    ImGui::Separator();
    if (registry.all_of<::engine::TransformComponent>(selectedEntity_)) {
        drawTransformInspector(selectedEntity_, registry);
    }
    if (registry.all_of<PhysicsComponent>(selectedEntity_)) {
        drawPhysicsInspector(selectedEntity_, registry);
    } else if (registry.all_of<ColliderComponent>(selectedEntity_)) {
        // show collider even if physics missing
        drawColliderInspector(selectedEntity_, registry);
    } else {
        // Still allow collider inspector if present without physics? handled above
        if (registry.all_of<ColliderComponent>(selectedEntity_)) drawColliderInspector(selectedEntity_, registry);
    }
    if (registry.all_of<PhysicsComponent>(selectedEntity_) && registry.all_of<ColliderComponent>(selectedEntity_)) {
        // both already drawn? Physics drawn, collider not yet if physics present we still need collider
        // Ensure collider drawn when both present (physics path skipped collider)
        // The above else chain skipped, so draw here if needed but avoid double draw
        // For simplicity, if both present, draw collider now if not already drawn
        // We track: if physics present, collider not yet drawn
        if (registry.all_of<ColliderComponent>(selectedEntity_)) {
            // Check if we already drew collider via the else branch: we didn't, because physics was true so we went to first branch
            // So draw now
            drawColliderInspector(selectedEntity_, registry);
        }
    }
    if (registry.all_of<BlackboardComponent>(selectedEntity_)) {
        drawBlackboardInspector(selectedEntity_, registry);
    }
    if (registry.all_of<::engine::GameplayComponent>(selectedEntity_)) {
        drawGameplayInspector(selectedEntity_, registry);
    }
    ImGui::Separator();
    if (ImGui::Button("+ Add Component")) {
        ImGui::OpenPopup("AddComponentPopup");
    }
    if (ImGui::BeginPopup("AddComponentPopup")) {
        if (!registry.all_of<PhysicsComponent>(selectedEntity_) && ImGui::MenuItem("Physics Component")) {
            registry.emplace<PhysicsComponent>(selectedEntity_);
        }
        if (!registry.all_of<ColliderComponent>(selectedEntity_) && ImGui::MenuItem("Collider Component")) {
            registry.emplace<ColliderComponent>(selectedEntity_);
        }
        if (!registry.all_of<BlackboardComponent>(selectedEntity_) && ImGui::MenuItem("Blackboard Component")) {
            registry.emplace<BlackboardComponent>(selectedEntity_);
        }
        if (!registry.all_of<::engine::TransformComponent>(selectedEntity_) && ImGui::MenuItem("Transform Component")) {
            registry.emplace<::engine::TransformComponent>(selectedEntity_);
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}

void SceneEditor::drawTransformInspector(entt::entity entity, entt::registry& registry) {
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto& transform = registry.get<::engine::TransformComponent>(entity);
        bool changed = false;
        changed |= ImGui::DragFloat3("Position", &transform.position[0], 0.1f);
        // Rotation as euler degrees for UX; engine stores radians
        glm::vec3 deg = glm::degrees(transform.rotation);
        if (ImGui::DragFloat3("Rotation (deg)", &deg[0], 0.5f)) {
            transform.rotation = glm::radians(deg);
            changed = true;
        }
        changed |= ImGui::DragFloat3("Scale", &transform.scale[0], 0.1f);
        if (changed && context_.state == SceneState::Edit) {
            if (registry.all_of<PhysicsComponent>(entity)) {
                auto& phys = registry.get<PhysicsComponent>(entity);
                phys.velocity = glm::vec3(0.0f);
            }
        }
    }
}

void SceneEditor::drawPhysicsInspector(entt::entity entity, entt::registry& registry) {
    if (ImGui::CollapsingHeader("Physics", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto& phys = registry.get<PhysicsComponent>(entity);
        ImGui::DragFloat("Mass", &phys.mass, 0.1f, 0.01f, 1000.0f);
        ImGui::DragFloat("Linear Damping", &phys.linearDamping, 0.1f, 0.0f, 100.0f);
        ImGui::Checkbox("Use Gravity", &phys.useGravity);
        ImGui::Checkbox("Is Grounded", &phys.isGrounded);
        ImGui::DragFloat3("Velocity", &phys.velocity[0], 0.1f);
    }
}

void SceneEditor::drawColliderInspector(entt::entity entity, entt::registry& registry) {
    if (ImGui::CollapsingHeader("Collider", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto& col = registry.get<ColliderComponent>(entity);
        const char* types[] = {"Sphere", "AABB"};
        int cur = (col.type == ColliderType::Sphere) ? 0 : 1;
        if (ImGui::Combo("Type", &cur, types, 2)) {
            col.type = (cur == 0) ? ColliderType::Sphere : ColliderType::AABB;
        }
        if (col.type == ColliderType::Sphere) {
            ImGui::DragFloat("Radius", &col.radius, 0.02f, 0.05f, 10.0f);
        } else {
            ImGui::DragFloat3("Half Extents", &col.halfExtents[0], 0.05f);
        }
        ImGui::DragFloat3("Center Offset", &col.centerOffset[0], 0.05f);
    }
}

void SceneEditor::drawBlackboardInspector(entt::entity entity, entt::registry& registry) {
    if (ImGui::CollapsingHeader("Blackboard", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto& bb = registry.get<BlackboardComponent>(entity);
        // Floats
        for (auto& [key, val] : bb.floats) {
            ImGui::DragFloat(key.c_str(), &val, 0.1f);
        }
        // Bools
        for (auto& [key, val] : bb.bools) {
            bool b = val;
            if (ImGui::Checkbox(key.c_str(), &b)) val = b;
        }
        // Vectors as drag
        for (auto& [key, val] : bb.vectors) {
            ImGui::DragFloat3(key.c_str(), &val[0], 0.1f);
        }
        ImGui::Separator();
        if (ImGui::Button("Add Float")) {
            int n = (int)bb.floats.size();
            bb.setFloat("Float" + std::to_string(n), 0.0f);
        }
        ImGui::SameLine();
        if (ImGui::Button("Add Bool")) {
            int n = (int)bb.bools.size();
            bb.setBool("Bool" + std::to_string(n), false);
        }
    }
}

void SceneEditor::drawGameplayInspector(entt::entity entity, entt::registry& registry) {
    if (ImGui::CollapsingHeader("Gameplay", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto& gc = registry.get<::engine::GameplayComponent>(entity);
        ImGui::Text("Graph: %s", gc.graph ? gc.graph->nodes.size() ? "assigned" : "empty" : "null");
        if (gc.graph) ImGui::Text("Nodes: %zu", gc.graph->nodes.size());
        if (registry.all_of<BlackboardComponent>(entity)) {
            auto& bb = registry.get<BlackboardComponent>(entity);
            if (bb.floats.count("TargetEntityID")) {
                ImGui::Text("TargetEntityID: %.0f", bb.floats["TargetEntityID"]);
            }
        }
    }
}

} // namespace Engine
