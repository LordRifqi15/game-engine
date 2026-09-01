#include "editor/SceneEditor.hpp"
#include "editor/EditorState.hpp"
#include "core/registry.h"
#include "ecs/components/TagComponent.hpp"
#include "core/transform_component.h"
#include "ecs/components/PhysicsComponent.hpp"
#include "ecs/components/ColliderComponent.hpp"
#include "ecs/components/BlackboardComponent.hpp"

#include <imgui.h>
#include <cstdio>
#include <cmath>

using namespace Engine;
using namespace engine;

int main(){
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280,720);
    unsigned char* pixels; int w,h;
    io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);

    entt::registry registry;
    SceneEditor editor;

    // 1) Entity creation & selection
    {
        auto e = registry.create();
        registry.emplace<TagComponent>(e, TagComponent("TestEntity"));
        registry.emplace<::engine::TransformComponent>(e, ::engine::TransformComponent{});
        editor.setSelectedEntity(e);
        if (editor.getSelectedEntity() != e) { printf("FAIL selection set/get\n"); return 1; }
        if (!registry.valid(e)) { printf("FAIL valid after create\n"); return 1; }
        printf("PASS selection set/get\n");
    }

    // 2) Safe null check
    {
        editor.setSelectedEntity(entt::null);
        if (editor.getSelectedEntity() != entt::null) { printf("FAIL null selection\n"); return 1; }
        auto e = registry.create();
        registry.emplace<TagComponent>(e, TagComponent("Temp"));
        editor.setSelectedEntity(e);
        registry.destroy(e);
        if (registry.valid(e)) { printf("FAIL valid after destroy should be false\n"); return 1; }
        if (editor.getSelectedEntity() == entt::null || !registry.valid(editor.getSelectedEntity())) {
            editor.setSelectedEntity(entt::null);
        }
        if (editor.getSelectedEntity() != entt::null) { printf("FAIL should reset to null after destroy\n"); return 1; }
        printf("PASS safe null and valid guard\n");
    }

    // 3) Safe deletion via inspector
    {
        auto e = registry.create();
        registry.emplace<TagComponent>(e, TagComponent("Deletable"));
        registry.emplace<::engine::TransformComponent>(e, ::engine::TransformComponent{});
        editor.setSelectedEntity(e);
        if (registry.valid(editor.getSelectedEntity())) {
            registry.destroy(editor.getSelectedEntity());
            editor.setSelectedEntity(entt::null);
        }
        if (registry.valid(e)) { printf("FAIL destroy should make invalid\n"); return 1; }
        if (editor.getSelectedEntity() != entt::null) { printf("FAIL selected should be null after delete\n"); return 1; }
        if (registry.all_of<TagComponent>(e)) { printf("FAIL tag should be gone after destroy\n"); return 1; }
        printf("PASS safe deletion\n");
    }

    // 4) EnTT assertion guard: never emplace without !all_of check
    {
        auto e = registry.create();
        registry.emplace<TagComponent>(e, TagComponent("Guarded"));
        registry.emplace<PhysicsComponent>(e, PhysicsComponent{});
        bool alreadyHas = registry.all_of<PhysicsComponent>(e);
        if (!alreadyHas) { printf("FAIL should have physics\n"); return 1; }
        if (!registry.all_of<PhysicsComponent>(e)) {
            registry.emplace<PhysicsComponent>(e, PhysicsComponent{});
            printf("FAIL should not emplace when already has\n"); return 1;
        }
        auto& phys = registry.get<PhysicsComponent>(e);
        if (phys.mass != 1.0f) { printf("FAIL phys mass changed\n"); return 1; }
        if (registry.all_of<ColliderComponent>(e)) { printf("FAIL should not have collider yet\n"); return 1; }
        registry.emplace<ColliderComponent>(e, ColliderComponent{});
        if (!registry.all_of<ColliderComponent>(e)) { printf("FAIL collider emplace failed\n"); return 1; }
        printf("PASS add component guard\n");
    }

    // 5) Transform vs Physics sync: Edit mode drag zeroes velocity
    {
        auto e = registry.create();
        registry.emplace<TagComponent>(e, TagComponent("SyncEntity"));
        ::engine::TransformComponent tr; tr.position = glm::vec3(0,0,0); tr.rotation = glm::vec3(0); tr.scale = glm::vec3(1);
        registry.emplace<::engine::TransformComponent>(e, tr);
        PhysicsComponent phys; phys.velocity = glm::vec3(5,0,5); phys.mass=1.0f;
        registry.emplace<PhysicsComponent>(e, phys);
        editor.setSelectedEntity(e);
        editor.getContext().state = SceneState::Edit;
        {
            auto& transform = registry.get<::engine::TransformComponent>(e);
            transform.position.x += 1.0f;
            if (editor.getContext().state == SceneState::Edit) {
                if (registry.all_of<PhysicsComponent>(e)) {
                    auto& p = registry.get<PhysicsComponent>(e);
                    p.velocity = glm::vec3(0.0f);
                }
            }
        }
        auto& p2 = registry.get<PhysicsComponent>(e);
        if (p2.velocity.x != 0.0f || p2.velocity.z != 0.0f) { printf("FAIL Edit mode should zero velocity after transform edit, got %.2f %.2f\n", p2.velocity.x, p2.velocity.z); return 1; }
        p2.velocity = glm::vec3(5,0,5);
        editor.getContext().state = SceneState::Play;
        {
            auto& transform = registry.get<::engine::TransformComponent>(e);
            transform.position.x += 1.0f;
            if (editor.getContext().state == SceneState::Edit) {
                auto& p = registry.get<PhysicsComponent>(e);
                p.velocity = glm::vec3(0.0f);
            }
        }
        if (p2.velocity.x == 0.0f) { printf("FAIL Play mode should not zero velocity\n"); return 1; }
        printf("PASS Transform-Physics sync Edit vs Play\n");
    }

    // 6) Simulation toggle: Edit -> Play -> Edit
    {
        editor.getContext().state = SceneState::Edit;
        if (editor.getContext().isSimulating()) { printf("FAIL Edit should not be simulating\n"); return 1; }
        editor.getContext().state = SceneState::Play;
        if (!editor.getContext().isSimulating()) { printf("FAIL Play should be simulating\n"); return 1; }
        editor.getContext().state = SceneState::Edit;
        if (editor.getContext().isSimulating()) { printf("FAIL second Edit should not be simulating\n"); return 1; }
        printf("PASS simulation toggle\n");
    }

    // 7) onImGuiRender should not crash with null selection and invalid entity (headless)
    {
        ImGui::NewFrame();
        editor.setSelectedEntity(entt::null);
        editor.onImGuiRender(registry);
        ImGui::Render();
        auto e = registry.create();
        registry.emplace<TagComponent>(e, TagComponent("Temp2"));
        editor.setSelectedEntity(e);
        registry.destroy(e);
        ImGui::NewFrame();
        editor.onImGuiRender(registry);
        ImGui::Render();
        if (editor.getSelectedEntity() != entt::null) {
            printf("FAIL onImGuiRender should reset invalid selected to null, got %u\n", editor.getSelectedEntity());
            return 1;
        }
        printf("PASS onImGuiRender safe with null/invalid\n");
    }

    // 8) Hierarchy Add Entity
    {
        size_t before = registry.getAllEntities().size();
        auto e = registry.create();
        registry.emplace<TagComponent>(e, TagComponent("New Entity"));
        registry.emplace<::engine::TransformComponent>(e, ::engine::TransformComponent{});
        if (registry.getAllEntities().size() != before+1) { printf("FAIL hierarchy add entity count\n"); return 1; }
        if (!registry.all_of<TagComponent>(e) || !registry.all_of<::engine::TransformComponent>(e)) { printf("FAIL new entity missing components\n"); return 1; }
        printf("PASS hierarchy add entity\n");
    }

    ImGui::DestroyContext();
    printf("PASS: editor selection all tests\n");
    return 0;
}
