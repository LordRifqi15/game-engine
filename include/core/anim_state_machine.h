#pragma once

#include "core/anim_graph.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace engine {

// One named state wrapping an animation graph.
struct AnimState {
    std::string name;
    std::shared_ptr<AnimGraph> graph;
    float timeInState = 0.0f; // machine-maintained, reset on entry
};

// Parameters conditions read each frame (set by game/engine code).
struct AnimParams {
    float speed = 0.0f;
    bool jumpPressed = false;
};

// Transition rule. from == nullptr means "Any state".
struct AnimTransition {
    AnimState* from = nullptr; // nullptr = Any
    AnimState* to = nullptr;
    std::function<bool(const AnimParams&, const AnimState&)> condition;
    float blendDuration = 0.3f;
};

// Selects the active graph, cross-fades between states via blendPoses.
// Graphs define motion; this machine defines intent.
class AnimStateMachine {
public:
    AnimState* addState(std::string name, std::shared_ptr<AnimGraph> graph);
    void addTransition(AnimState* from, AnimState* to,
                       std::function<bool(const AnimParams&, const AnimState&)> condition,
                       float blendDuration = 0.3f);
    bool setStateGraph(const std::string& name, std::shared_ptr<AnimGraph> g);
    void setStart(AnimState* s) { current_ = s; }
    const AnimState* current() const { return current_; }
    AnimParams& params() { return params_; }

    // Advance: check transitions, evaluate active graph (blending with the
    // previous state's graph during cross-fade). Writes blended pose.
    void update(float dt, std::vector<Skeleton::Pose>& outPose);

    // Convenience: update + write into skeleton.pose + finalMatrices.
    void evaluateInto(Skeleton& skeleton, float dt);

private:
    std::vector<std::unique_ptr<AnimState>> states_;
    std::vector<AnimTransition> transitions_;
    AnimState* current_ = nullptr;
    // Cross-fade bookkeeping
    AnimState* prev_ = nullptr;
    float blendTime_ = 0.0f;
    float blendDuration_ = 0.3f;
    AnimParams params_;
};

// Demo machine per spec: Idle <-> Locomotion (speed), Any -> Jump
// (jumpPressed), Jump -> Idle (clip finished via timeInState).
// anims: [0]=idle-ish clip, [1]=locomotion source, [2]=jump (falls back if absent).
std::shared_ptr<AnimStateMachine> makeDefaultStateMachine(const std::vector<Animation>& anims,
                                                          const Skeleton& baseSkeleton,
                                                          float jumpDuration);

} // namespace engine
