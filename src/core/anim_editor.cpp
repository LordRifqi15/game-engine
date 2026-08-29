#include "core/anim_editor.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <set>

namespace engine {

EditorNode* EditorGraph::find(int id) {
    for (auto& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}

const EditorNode* EditorGraph::find(int id) const {
    for (const auto& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}

bool EditorGraph::removeNode(int id) {
    auto it = std::find_if(nodes.begin(), nodes.end(), [id](const EditorNode& n) { return n.id == id; });
    if (it == nodes.end()) return false;
    nodes.erase(it);
    links.erase(std::remove_if(links.begin(), links.end(),
                               [id](const NodeLink& l) { return l.fromNode == id || l.toNode == id; }),
                links.end());
    if (outputNode == id) outputNode = -1;
    return true;
}

void EditorGraph::removeLink(int fromNode, int toNode, int toSlot) {
    links.erase(std::remove_if(links.begin(), links.end(), [=](const NodeLink& l) {
                    return l.fromNode == fromNode && l.toNode == toNode && l.toSlot == toSlot;
                }),
                links.end());
}

EditorBuildResult buildRuntimeGraph(const EditorGraph& ed, const Skeleton& baseSkeleton) {
    EditorBuildResult res;
    auto graph = std::make_shared<AnimGraph>();
    graph->baseSkeleton = baseSkeleton;
    if (graph->baseSkeleton.pose.size() != graph->baseSkeleton.joints.size()) graph->baseSkeleton.resizePose();

    const EditorNode* outNode = ed.find(ed.outputNode);
    if (!outNode) { res.error = "no output node selected"; return res; }

    std::map<int, std::vector<int>> deps;
    for (const auto& l : ed.links) deps[l.toNode].push_back(l.fromNode);
    std::function<bool(int, std::set<int>&)> cyclic = [&](int n, std::set<int>& path) -> bool {
        if (path.count(n)) return true;
        path.insert(n);
        auto it = deps.find(n);
        if (it != deps.end())
            for (int from : it->second)
                if (cyclic(from, path)) return true;
        path.erase(n);
        return false;
    };
    std::set<int> path;
    if (cyclic(ed.outputNode, path)) { res.error = "graph has a cycle"; return res; }

    std::map<int, AnimNode*> runtime;
    std::map<int, FloatParameterNode*> params;
    for (const auto& n : ed.nodes) {
        if (n.type == "Clip") {
            Animation clipData = n.clip;
            auto clip = std::make_unique<ClipNode>(clipData);
            clip->playbackSpeed = 1.0f;
            runtime[n.id] = clip.get();
            graph->ownedNodes.push_back(std::move(clip));
        } else if (n.type == "Blend") {
            auto blend = std::make_unique<BlendNode>();
            blend->inMin = n.inMin;
            blend->inMax = n.inMax;
            blend->blendDuration = n.blendDuration;
            runtime[n.id] = blend.get();
            graph->ownedNodes.push_back(std::move(blend));
        } else if (n.type == "Param") {
            auto p = std::make_unique<FloatParameterNode>();
            p->value = n.value;
            params[n.id] = p.get();
            graph->ownedParams.push_back(std::move(p));
        }
    }

    for (const auto& l : ed.links) {
        auto* toBlend = dynamic_cast<BlendNode*>(runtime[l.toNode]);
        if (!toBlend) continue;
        const EditorNode* from = ed.find(l.fromNode);
        if (!from) continue;
        if (from->type == "Param") {
            auto it = params.find(l.fromNode);
            if (it != params.end() && l.toSlot == 2) toBlend->param = it->second;
        } else {
            auto it = runtime.find(l.fromNode);
            if (it == runtime.end()) continue;
            if (l.toSlot == 0) toBlend->inputA = it->second;
            else if (l.toSlot == 1) toBlend->inputB = it->second;
        }
    }

    graph->root = runtime[ed.outputNode];
    if (!graph->root) { res.error = "output node is not a pose node"; return res; }
    res.graph = std::move(graph);
    return res;
}

EditorBuildResult buildRuntimeGraph(const EditorGraph& ed, const Skeleton& baseSkeleton,
                                    const std::vector<Animation>& anims) {
    EditorGraph resolved = ed;
    for (auto& n : resolved.nodes) {
        if (n.type != "Clip" || n.clipIndex < 0) continue;
        if (n.clipIndex < (int)anims.size()) {
            n.clip = anims[n.clipIndex];
        } else if (!anims.empty()) {
            // Missing clip -> fallback to last available, warn.
            std::fprintf(stderr, "[anim] clip index %d missing (have %zu), falling back to last\n",
                         n.clipIndex, anims.size());
            n.clip = anims.back();
        }
    }
    return buildRuntimeGraph(resolved, baseSkeleton);
}


EditorGraph makeLocomotionEditorGraph(const std::vector<Animation>& anims) {
    EditorGraph g;
    EditorNode& param = g.nodes.emplace_back();
    param.id = g.nextId(); param.type = "Param"; param.name = "Speed";
    param.value = 0.0f; param.x = 480.0f; param.y = 200.0f;
    int paramId = param.id;

    auto addClip = [&](const Animation& a, int clipIdx, float x, float y) {
        EditorNode& n = g.nodes.emplace_back();
        n.id = g.nextId(); n.type = "Clip";
        n.name = a.name.empty() ? "Clip" : a.name;
        n.clip = a; n.clipIndex = clipIdx; n.x = x; n.y = y;
        return n.id;
    };
    int idleId = addClip(anims.size() > 0 ? anims[0] : Animation{}, 0, 40.0f, 40.0f);
    int walkId = addClip(anims.size() > 1 ? anims[1] : Animation{}, 1, 40.0f, 220.0f);
    int runId  = addClip(anims.size() > 2 ? anims[2] : (anims.size() > 1 ? anims[1] : Animation{}), anims.size() > 2 ? 2 : (anims.size() > 1 ? 1 : -1), 40.0f, 400.0f);

    auto addBlend = [&](float x, float y, float mn, float mx) {
        EditorNode& n = g.nodes.emplace_back();
        n.id = g.nextId(); n.type = "Blend";
        n.inMin = mn; n.inMax = mx; n.x = x; n.y = y;
        return n.id;
    };
    int walkRunId = addBlend(280.0f, 260.0f, 1.5f, 2.5f);
    int finalId   = addBlend(520.0f, 120.0f, 0.1f, 1.0f);

    g.links.push_back({idleId, finalId, 0});
    g.links.push_back({walkRunId, finalId, 1});
    g.links.push_back({walkId, walkRunId, 0});
    g.links.push_back({runId, walkRunId, 1});
    g.links.push_back({paramId, walkRunId, 2});
    g.links.push_back({paramId, finalId, 2});
    g.outputNode = finalId;
    return g;
}

} // namespace engine
