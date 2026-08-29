// Test for Task 033: prompt -> JSON -> EditorGraph -> runtime, deterministic.
// Build: g++ -std=c++17 -I../include ../src/core/animation_system.cpp ../src/core/anim_graph.cpp ../src/core/anim_editor.cpp ../src/core/anim_graph_asset.cpp ../src/core/anim_graph_ai.cpp anim_ai_test.cpp -o anim_ai_test

#include "core/anim_graph_ai.h"
#include "core/anim_graph_asset.h"

#include <cmath>
#include <cstdio>

using namespace engine;

static Animation makeClip(float angle, const std::string& name) {
    Animation a;
    a.name = name;
    a.duration = 1.0f;
    AnimationSampler s;
    s.interpolation = "LINEAR";
    glm::quat q = glm::angleAxis(angle, glm::vec3(0, 1, 0));
    s.inputs = {0.0f};
    s.outputs = {glm::vec4(q.x, q.y, q.z, q.w)};
    a.samplers.push_back(s);
    AnimationChannel ch; ch.targetJoint = 0; ch.path = "rotation"; ch.samplerIndex = 0;
    a.channels.push_back(ch);
    return a;
}

static float yawOf(const Skeleton::Pose& p) {
    return 2.0f * std::atan2(p.rotation.y, p.rotation.w);
}

int main() {
    // 1. Deterministic: same prompt -> identical string.
    std::string a = generateGraphJSON("idle walk run with jump");
    std::string b = generateGraphJSON("Idle Walk Run With Jump"); // case-insensitive
    if (a != b) { std::printf("FAIL: not deterministic (case)\n"); return 1; }

    // 2. Valid JSON: loads into EditorGraph.
    EditorGraph g;
    if (!loadGraphFromString(g, a)) { std::printf("FAIL: generated JSON rejected\n%s\n", a.c_str()); return 1; }
    if (g.outputNode < 0) { std::printf("FAIL: no output\n"); return 1; }
    if (g.nodes.size() != 8) { std::printf("FAIL: expected 8 nodes (param+4clips+3blends), got %zu\n", g.nodes.size()); return 1; }
    if (g.links.size() != 9) { std::printf("FAIL: expected 9 links, got %zu\n", g.links.size()); return 1; }
    // 3. Runs: build with 4 anims, speed sweep produces expected poses.
    Skeleton base; Joint j; j.parent = -1; base.joints = {j}; base.resizePose();
    std::vector<Animation> anims{makeClip(0.0f, "Idle"), makeClip(1.0f, "Walk"),
                                 makeClip(2.0f, "Run"), makeClip(0.5f, "Jump")};
    auto res = buildRuntimeGraph(g, base, anims);
    if (!res.graph) { std::printf("FAIL build: %s\n", res.error.c_str()); return 1; }
    std::vector<Skeleton::Pose> out;
    res.graph->setSpeed(0.0f);
    for (int i = 0; i < 120; ++i) res.graph->evaluate(1.0f / 60.0f, out);
    if (std::abs(yawOf(out[0])) > 0.05f) { std::printf("FAIL idle yaw %.3f\n", yawOf(out[0])); return 1; }
    // Walk->Run blend range 1.5-2.5: speed 2.0 -> alpha 0.5 -> slerp yaw 1.0->2.0 = 1.5
    res.graph->setSpeed(2.0f);
    for (int i = 0; i < 120; ++i) res.graph->evaluate(1.0f / 60.0f, out);
    if (std::abs(yawOf(out[0]) - 1.5f) > 0.1f) { std::printf("FAIL mid-run yaw %.3f\n", yawOf(out[0])); return 1; }
    // Speed 3.5 -> Run->Jump blend (3.0-4.0) alpha 0.5 -> yaw 2.0->0.5 = 1.25
    res.graph->setSpeed(3.5f);
    for (int i = 0; i < 120; ++i) res.graph->evaluate(1.0f / 60.0f, out);
    if (std::abs(yawOf(out[0]) - 1.25f) > 0.15f) { std::printf("FAIL jump-blend yaw %.3f\n", yawOf(out[0])); return 1; }


    // 4. Missing clips fallback: only 2 anims -> jump/run indices clamp to last, no crash.
    std::vector<Animation> few{makeClip(0.0f, "Idle"), makeClip(1.0f, "Walk")};
    auto res2 = buildRuntimeGraph(g, base, few);
    if (!res2.graph) { std::printf("FAIL fallback build\n"); return 1; }
    res2.graph->setSpeed(3.5f);
    for (int i = 0; i < 60; ++i) res2.graph->evaluate(1.0f / 60.0f, out); // must not crash

    // 5. Invalid JSON rejected.
    EditorGraph bad;
    if (loadGraphFromString(bad, "{not json")) { std::printf("FAIL: invalid JSON accepted\n"); return 1; }

    // 6. Simpler prompts produce smaller graphs.
    std::string simple = generateGraphJSON("idle walk");
    EditorGraph sg;
    if (!loadGraphFromString(sg, simple)) { std::printf("FAIL simple load\n"); return 1; }
    if (sg.nodes.size() >= g.nodes.size()) { std::printf("FAIL: simple not smaller\n"); return 1; }

    std::printf("PASS: deterministic generation, valid JSON, runs, fallback, reject\n");
    return 0;
}
