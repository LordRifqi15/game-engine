#pragma once

#include "core/anim_graph.h"

#include <string>
#include <vector>

namespace engine {

// UI-agnostic editor model (Task 031). The ImGui layer renders this;
// toRuntimeGraph() converts it to the runtime AnimGraph.
struct EditorNode {
    int id = 0;
    std::string type;   // "Clip", "Blend", "Param"
    float x = 0.0f, y = 0.0f; // position
    // Per-type payload
    std::string name;         // Clip name / Param name
    Animation clip;           // Clip: full data (for self-contained files)
    int clipIndex = -1;       // Clip: index into external animations (spec's "clip": 0)
    float inMin = 0.0f, inMax = 1.0f; // Blend speed range
    float blendDuration = 0.3f;       // Blend smoothing
    float value = 0.0f;               // Param current value
};


// Connection: one output pin -> one input pin. Blend has slots 0(A), 1(B), 2(param).
struct NodeLink {
    int fromNode = -1;
    int toNode = -1;
    int toSlot = 0;
};

struct EditorGraph {
    std::vector<EditorNode> nodes;
    std::vector<NodeLink> links;
    int outputNode = -1; // node id whose pose is the graph output

    EditorNode* find(int id);
    const EditorNode* find(int id) const;
    int nextId() { return ++lastId_; }
    void setNextId(int id) { lastId_ = id; }
    bool removeNode(int id); // removes node + attached links
    void removeLink(int fromNode, int toNode, int toSlot);

private:
    int lastId_ = 0;
};



struct EditorBuildResult {
    std::shared_ptr<AnimGraph> graph;
    std::string error; // empty on success
};

// Convert editor graph to a runtime AnimGraph. Fails on: no output,
// unknown node refs, or dependency cycles (would recurse forever).
EditorBuildResult buildRuntimeGraph(const EditorGraph& ed, const Skeleton& baseSkeleton);
// Overload that resolves Clip nodes with clipIndex via provided animations
// (for index-based JSON like spec's "clip": 0).
EditorBuildResult buildRuntimeGraph(const EditorGraph& ed, const Skeleton& baseSkeleton,
                                    const std::vector<Animation>& anims);


// Mirror of makeLocomotionGraph so the editor opens with the known-good
// locomotion setup (Param + Blend(Idle, Blend(Walk, Run))).
EditorGraph makeLocomotionEditorGraph(const std::vector<Animation>& anims);

} // namespace engine
