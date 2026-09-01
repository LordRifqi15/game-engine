#pragma once

namespace Engine {

enum class SceneState {
    Edit,
    Play,
    Pause
};

struct EditorContext {
    SceneState state{SceneState::Edit};
    bool isSimulating() const { return state == SceneState::Play; }
};

} // namespace Engine

namespace engine {
    using SceneState = ::Engine::SceneState;
    using EditorContext = ::Engine::EditorContext;
}
