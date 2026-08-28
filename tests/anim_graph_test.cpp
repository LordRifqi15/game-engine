// Minimal self-check for Task 029 anim graph: builds a 2-clip graph with
// synthetic rotation clips and asserts speed-driven blending converges.
// Build: g++ -std=c++17 -I../include ../src/core/animation_system.cpp ../src/core/anim_graph.cpp anim_graph_test.cpp -o anim_graph_test
// Run:   ./anim_graph_test  (exits non-zero on failure)

#include "core/anim_graph.h"

#include <cmath>
#include <cstdio>

using namespace engine;

static Animation makeRotClip(float angleRad, const std::string& name) {
    Animation a;
    a.name = name;
    a.duration = 1.0f;
    AnimationSampler s;
    s.interpolation = "LINEAR";
    // Single keyframe = static pose (clip holds target rotation).
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

// Angle of pose rotation around Y (quaternion w,x,y,z).
static float yawOf(const Skeleton::Pose& p) {
    return 2.0f * std::atan2(p.rotation.y, p.rotation.w);
}

int main() {
    Skeleton base;
    Joint j0, j1;
    j0.parent = -1; j1.parent = 0;
    base.joints = {j0, j1};
    base.resizePose();

    std::vector<Animation> clips;
    clips.push_back(makeRotClip(0.0f, "Idle"));        // yaw 0
    clips.push_back(makeRotClip(1.0f, "Walk"));        // yaw 1.0 rad

    auto g = makeLocomotionGraph(clips, base);
    if (!g->root) { std::printf("FAIL: no root\n"); return 1; }

    std::vector<Skeleton::Pose> out;

    // Speed 0 (Idle): after settling, yaw ~0.
    g->setSpeed(0.0f);
    for (int i = 0; i < 60; ++i) g->evaluate(1.0f / 60.0f, out);
    if (std::abs(yawOf(out[0])) > 0.05f) { std::printf("FAIL idle yaw %.3f\n", yawOf(out[0])); return 1; }

    // Speed 2.5 (Run -> falls back to clip1): alpha converges to 1, yaw ~1.0.
    g->setSpeed(2.5f);
    for (int i = 0; i < 120; ++i) g->evaluate(1.0f / 60.0f, out);
    if (std::abs(yawOf(out[0]) - 1.0f) > 0.05f) { std::printf("FAIL run yaw %.3f\n", yawOf(out[0])); return 1; }

    // Mid speed 0.55: final blend alpha = (0.55-0.1)/(1.0-0.1) = 0.5 -> slerp yaw ~0.5.
    g->setSpeed(0.55f);
    for (int i = 0; i < 120; ++i) g->evaluate(1.0f / 60.0f, out);
    float y = yawOf(out[0]);
    if (std::abs(y - 0.5f) > 0.1f) { std::printf("FAIL mid yaw %.3f (want ~0.5)\n", y); return 1; }

    // No snap: jump speed 0.55 -> 0 in one frame must move yaw < 0.2.
    float before = y;
    g->setSpeed(0.0f);
    g->evaluate(1.0f / 60.0f, out);
    float after = yawOf(out[0]);
    if (std::abs(after - before) > 0.2f) { std::printf("FAIL snap %.3f -> %.3f\n", before, after); return 1; }

    // evaluateInto writes finalMatrices (global*inverseBind, identity invbind -> global).
    Skeleton skel = base;
    g->evaluateInto(skel, 1.0f / 60.0f);
    if (skel.finalMatrices.size() != base.joints.size()) { std::printf("FAIL matrices size\n"); return 1; }

    std::printf("PASS: graph blend idle/mid/run, no snap, matrices ok\n");
    return 0;
}
