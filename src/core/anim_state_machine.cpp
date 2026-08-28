#include "core/anim_state_machine.h"
#include "core/animation_system.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace engine {

AnimState* AnimStateMachine::addState(std::string name, std::shared_ptr<AnimGraph> graph) {
    auto s = std::make_unique<AnimState>();
    s->name = std::move(name);
    s->graph = std::move(graph);
    AnimState* ptr = s.get();
    states_.push_back(std::move(s));
    if (!current_) current_ = ptr; // first added state is default start
    return ptr;
}

void AnimStateMachine::addTransition(AnimState* from, AnimState* to,
                                     std::function<bool(const AnimParams&, const AnimState&)> condition,
                                     float blendDuration) {
    transitions_.push_back(AnimTransition{from, to, std::move(condition), blendDuration > 0.0f ? blendDuration : 0.3f});
}

void AnimStateMachine::update(float dt, std::vector<Skeleton::Pose>& outPose) {
    if (!current_) { outPose.clear(); return; }
    current_->timeInState += dt;

    // Check transitions from current (and Any) — first match wins.
    for (const AnimTransition& t : transitions_) {
        if (t.from && t.from != current_) continue;
        if (!t.to || t.to == current_ || !t.condition) continue;
        if (t.condition(params_, *current_)) {
            std::printf("[state] %s -> %s (blend %.2fs)\n", current_->name.c_str(), t.to->name.c_str(), t.blendDuration);
            std::fflush(stdout);
            prev_ = current_;
            current_ = t.to;
            current_->timeInState = 0.0f;
            blendTime_ = 0.0f;
            blendDuration_ = t.blendDuration;
            break; // one transition per frame
        }
    }

    // Evaluate active graph.
    current_->graph->setSpeed(params_.speed);
    current_->graph->evaluate(dt, outPose);

    // Cross-fade from previous state's graph (dual-pose blend, reused blendPoses).
    if (prev_ && prev_->graph) {
        blendTime_ += dt;
        float a = blendDuration_ > 0.0f ? std::min(blendTime_ / blendDuration_, 1.0f) : 1.0f;
        prev_->graph->setSpeed(params_.speed);
        std::vector<Skeleton::Pose> prevPose;
        prev_->graph->evaluate(dt, prevPose);
        std::vector<Skeleton::Pose> blended;
        blendPoses(prevPose, outPose, a, blended);
        outPose = std::move(blended);
        if (a >= 1.0f) prev_ = nullptr;
    }
}

void AnimStateMachine::evaluateInto(Skeleton& skeleton, float dt) {
    std::vector<Skeleton::Pose> blended;
    update(dt, blended);
    if (blended.empty()) return;
    if (blended.size() != skeleton.joints.size()) skeleton.resizePose();
    skeleton.pose = std::move(blended);
    computeFinalMatrices(skeleton);
}

std::shared_ptr<AnimStateMachine> makeDefaultStateMachine(const std::vector<Animation>& anims,
                                                          const Skeleton& baseSkeleton,
                                                          float jumpDuration) {
    auto sm = std::make_shared<AnimStateMachine>();
    if (anims.empty()) return sm;

    const Animation& idleClip = anims[0];
    const Animation& moveClip = anims.size() > 1 ? anims[1] : anims[0];
    const Animation& jumpClip = anims.size() > 2 ? anims[2] : moveClip;

    AnimState* idle = sm->addState("Idle", makeSingleClipGraph(idleClip, baseSkeleton));
    AnimState* loco = sm->addState("Locomotion", makeLocomotionGraph(anims, baseSkeleton));
    AnimState* jump = sm->addState("Jump", makeSingleClipGraph(jumpClip, baseSkeleton, 1.5f));
    sm->setStart(idle);

    // Idle <-> Locomotion by speed (spec thresholds).
    sm->addTransition(idle, loco,
                      [](const AnimParams& p, const AnimState&) { return p.speed > 0.1f; }, 0.3f);
    sm->addTransition(loco, idle,
                      [](const AnimParams& p, const AnimState&) { return p.speed < 0.1f; }, 0.3f);
    // Any -> Jump on edge-triggered jump.
    sm->addTransition(nullptr, jump,
                      [](const AnimParams& p, const AnimState&) { return p.jumpPressed; }, 0.15f);
    // Jump -> Idle when the jump clip has played through.
    float jd = jumpDuration > 0.0f ? jumpDuration : 1.0f;
    sm->addTransition(jump, idle,
                      [jd](const AnimParams&, const AnimState& s) { return s.timeInState >= jd; }, 0.3f);
    return sm;
}

} // namespace engine
