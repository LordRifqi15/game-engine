#pragma once

#include "core/gameplay_graph.h"

#include <memory>

namespace engine {

// Per-entity gameplay behavior. Each entity owns its own graph instance
// so node state (timers, edge detectors, blend alphas) is isolated.
struct GameplayComponent {
    std::shared_ptr<GameplayGraph> graph;

    GameplayComponent() = default;
    explicit GameplayComponent(std::shared_ptr<GameplayGraph> g) : graph(std::move(g)) {}
};

// Helper to clone a gameplay graph for a new entity (fresh state).
inline std::shared_ptr<GameplayGraph> cloneGameplayGraph(const GameplayGraph& src, AnimParams* newTarget) {
    return src.clone(newTarget);
}

} // namespace engine
