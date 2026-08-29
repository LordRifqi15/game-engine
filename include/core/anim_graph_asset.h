#pragma once

#include "core/anim_editor.h"

#include <string>

namespace engine {

// Asset system for animation graphs (Task 032).
// EditorGraph <-> JSON File <-> Runtime AnimGraph
// Runtime (AnimGraph) never includes editor UI; this header is the bridge
// used by editor and engine startup. Editor depends on runtime.

bool saveGraph(const EditorGraph& graph, const std::string& path);
bool loadGraph(EditorGraph& outGraph, const std::string& path);

// Helpers for engine integration: load + build runtime in one step.
// Returns nullptr on failure.
std::shared_ptr<AnimGraph> loadGraphAsRuntime(const std::string& path, const Skeleton& baseSkeleton);

// Overload that resolves clip indices using provided animations (for index-based files).
std::shared_ptr<AnimGraph> loadGraphAsRuntime(const std::string& path, const Skeleton& baseSkeleton,
                                              const std::vector<Animation>& anims);

} // namespace engine
