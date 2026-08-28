// Self-check for Task 030 state machine: transitions, Any->Jump,
// Jump->Idle on finish, smooth cross-state blend.
// Build: g++ -std=c++17 -I../include ../src/core/animation_system.cpp ../src/core/anim_graph.cpp ../src/core/anim_state_machine.cpp anim_sm_test.cpp -o anim_sm_test

#include "core/anim_state_machine.h"

#include <cmath>
#include <cstdio>

using namespace engine;

static Animation makeRotClip(float angleRad, const std::string& name, float dur = 1.0f) {
    Animation a;
    a.name = name;
    a.duration = dur;
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
    clips.push_back(makeRotClip(0.0f, "Idle"));      // yaw 0
    clips.push_back(makeRotClip(1.0f, "Move"));      // yaw 1
    clips.push_back(makeRotClip(0.5f, "Jump", 0.6f));// yaw 0.5, 0.6s

    auto sm = makeDefaultStateMachine(clips, base, 0.6f);
    if (!sm->current()) { std::printf("FAIL: no current state\n"); return 1; }
    if (sm->current()->name != "Idle") { std::printf("FAIL: start not Idle\n"); return 1; }

    std::vector<Skeleton::Pose> out;
    const float dt = 1.0f / 60.0f;

    // Idle holds yaw 0.
    for (int i = 0; i < 30; ++i) sm->update(dt, out);
    if (std::abs(yawOf(out[0])) > 0.02f) { std::printf("FAIL idle yaw %.3f\n", yawOf(out[0])); return 1; }

    // Speed > 0.1 -> transition to Locomotion, blend converges to Move yaw 1.
    sm->params().speed = 1.0f;
    for (int i = 0; i < 90; ++i) sm->update(dt, out);
    if (sm->current()->name != "Locomotion") { std::printf("FAIL: not Locomotion, is %s\n", sm->current()->name.c_str()); return 1; }
    if (std::abs(yawOf(out[0]) - 1.0f) > 0.05f) { std::printf("FAIL loco yaw %.3f\n", yawOf(out[0])); return 1; }
    sm->params().jumpPressed = true;
    float before = yawOf(out[0]);
    sm->update(dt, out);
    sm->params().jumpPressed = false; // edge consumed
    float after = yawOf(out[0]);
    if (std::abs(after - before) > 0.2f) { std::printf("FAIL snap %.3f -> %.3f\n", before, after); return 1; }
    if (sm->current()->name != "Jump") { std::printf("FAIL: jump not entered, is %s\n", sm->current()->name.c_str()); return 1; }

    // Jump -> Idle after clip finishes (0.6s). Speed 0 so Idle holds.
    sm->params().speed = 0.0f;
    for (int i = 0; i < 60; ++i) sm->update(dt, out);
    if (sm->current()->name != "Idle") { std::printf("FAIL: not back to Idle, is %s\n", sm->current()->name.c_str()); return 1; }

    // Any->Jump works from Locomotion too.
    sm->params().speed = 1.0f;
    for (int i = 0; i < 60; ++i) sm->update(dt, out); // reach Locomotion
    sm->params().jumpPressed = true;
    sm->update(dt, out);
    if (sm->current()->name != "Jump") { std::printf("FAIL: any->jump from Loco failed, is %s\n", sm->current()->name.c_str()); return 1; }

    std::printf("PASS: state machine transitions + any-jump + finish + no snap\n");
    return 0;
}
