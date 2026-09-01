#pragma once

#include "core/registry.h"
#include "editor/EditorState.hpp"

#include <filesystem>

namespace Engine {

class SceneEditor {
public:
    SceneEditor() = default;
    ~SceneEditor() = default;

    void onImGuiRender(entt::registry& registry);

    void setSelectedEntity(entt::entity entity) { selectedEntity_ = entity; }
    entt::entity getSelectedEntity() const { return selectedEntity_; }
    EditorContext& getContext() { return context_; }
    const EditorContext& getContext() const { return context_; }

    void setScenePath(const std::filesystem::path& p) { currentScenePath_ = p; }
    const std::filesystem::path& getScenePath() const { return currentScenePath_; }

private:
    void drawMenuBar(entt::registry& registry);
    void drawHierarchy(entt::registry& registry);
    void drawInspector(entt::registry& registry);

    void drawTransformInspector(entt::entity entity, entt::registry& registry);
    void drawPhysicsInspector(entt::entity entity, entt::registry& registry);
    void drawColliderInspector(entt::entity entity, entt::registry& registry);
    void drawBlackboardInspector(entt::entity entity, entt::registry& registry);
    void drawGameplayInspector(entt::entity entity, entt::registry& registry);

    entt::entity selectedEntity_{entt::null};
    EditorContext context_;
    std::filesystem::path currentScenePath_{"assets/scenes/default.scene.json"};
};

} // namespace Engine

namespace engine {
    using SceneEditor = ::Engine::SceneEditor;
}
