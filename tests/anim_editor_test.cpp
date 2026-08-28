// Self-check for Task 031 editor core: build runtime graph from editor model.
// Build: g++ -std=c++17 -I../include ../src/core/animation_system.cpp ../src/core/anim_graph.cpp ../src/core/anim_editor.cpp anim_editor_test.cpp -o anim_editor_test

#include "core/anim_editor.h"

#include <cmath>
#include <cstdio>

using namespace engine;

static Animation makeRotClip(float angleRad, const std::string& name) {
    Animation a;
    a.name = name;
    a.duration = 1.0f;
    AnimationSampler s;
    s.interpolation = "LINEAR";
    glm::quat q = glm::angleAxis(angleRad, glm::vec3(0.0f, 1.0f, 0.0f));
    s.inputs = {0.0f};
    s.outputs = {glm::vec4(q.x, q.y, q.z, q.w)};
    a.samplers.push_back(std::move(s));
    AnimationChannel ch;
    ch.targetJoint = 0;
    ch.path = "rotation";
    ch.samplerIndex = 0;
    a.channels.push_back(ch);
    return a;
}

static float yawOf(const Skeleton::Pose& p) {
    return 2.0f * std::atan2(p.rotation.y, p.rotation.w);
}

int main() {
    Skeleton base;
    Joint j0; j0.parent = -1;
    base.joints = {j0};
    base.resizePose();

    std::vector<Animation> clips;
    clips.push_back(makeRotClip(0.0f, "Idle"));
    clips.push_back(makeRotClip(1.0f, "Walk"));

    // Default locomotion editor graph builds and evaluates like the code-built one.
    EditorGraph ed = makeLocomotionEditorGraph(clips);
    auto built = buildRuntimeGraph(ed, base);
    if (!built.graph) { std::printf("FAIL build: %s\n", built.error.c_str()); return 1; }

    std::vector<Skeleton::Pose> out;
    built.graph->setSpeed(0.0f);
    for (int i = 0; i < 90; ++i) built.graph->evaluate(1.0f / 60.0f, out);
    if (std::abs(yawOf(out[0])) > 0.05f) { std::printf("FAIL idle yaw %.3f\n", yawOf(out[0])); return 1; }
    built.graph->setSpeed(1.0f);
    for (int i = 0; i < 120; ++i) built.graph->evaluate(1.0f / 60.0f, out);
    if (std::abs(yawOf(out[0]) - 1.0f) > 0.05f) { std::printf("FAIL walk yaw %.3f\n", yawOf(out[0])); return 1; }

    // Param node edit propagates to runtime (value mirrored at build time).
    EditorNode* p = ed.find(1); // first node is the Param
    if (p) p->value = 0.5f;
    built = buildRuntimeGraph(ed, base);
    if (!built.graph) { std::printf("FAIL rebuild: %s\n", built.error.c_str()); return 1; }
    // runtime param node holds 0.5; graph->speed untouched
    if (built.graph->ownedParams.empty() || std::abs(built.graph->ownedParams[0]->value - 0.5f) > 1e-5f) {
        std::printf("FAIL param value\n"); return 1;
    }

    // Cycle detection: link final output back into a blend input.
    EditorGraph cyc = makeLocomotionEditorGraph(clips);
    cyc.links.push_back({cyc.outputNode, 5, 0}); // 5 = walkRun blend (id order: 1=Param,2..4=clips,5,6=blends)
    built = buildRuntimeGraph(cyc, base);
    if (built.graph || built.error.empty()) { std::printf("FAIL: cycle not detected\n"); return 1; }

    // Node removal cleans links.
    EditorGraph rm = makeLocomotionEditorGraph(clips);
    int linksBefore = (int)rm.links.size();
    rm.removeNode(2); // first clip
    if ((int)rm.links.size() >= linksBefore) { std::printf("FAIL: links not cleaned\n"); return 1; }

    std::printf("PASS: editor->runtime build, param edit, cycle detect, node removal\n");
    return 0;
}
