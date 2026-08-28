#pragma once

#include "core/skeleton.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace engine {

// Base node: evaluates to a pose vector for the skeleton.
struct AnimNode {
    virtual ~AnimNode() = default;
    virtual void evaluate(float dt, const Skeleton& baseSkeleton, std::vector<Skeleton::Pose>& out) = 0;
};

// Holds a mutable float (speed, etc.). Not a pose evaluator itself.
struct FloatParameterNode {
    float value = 0.0f;
};

// Plays a single animation clip. Time advances with dt.
struct ClipNode : AnimNode {
    Animation clip;
    float time = 0.0f;
    float playbackSpeed = 1.0f;
    bool loop = true;

    explicit ClipNode(const Animation& anim) : clip(anim) {}
    explicit ClipNode(Animation&& anim) : clip(std::move(anim)) {}

    void evaluate(float dt, const Skeleton& baseSkeleton, std::vector<Skeleton::Pose>& out) override;
};

// Blend between two pose sources with alpha derived from a FloatParameterNode.
// Alpha is smoothed toward target over blendDuration (0.3s default) for no snap.
struct BlendNode : AnimNode {
    AnimNode* inputA = nullptr;
    AnimNode* inputB = nullptr;
    FloatParameterNode* param = nullptr;
    float inMin = 0.0f;
    float inMax = 1.0f;
    float blendDuration = 0.3f;
    float currentAlpha = 0.0f;

    BlendNode() = default;
    BlendNode(AnimNode* a, AnimNode* b, FloatParameterNode* p, float mn, float mx, float dur = 0.3f)
        : inputA(a), inputB(b), param(p), inMin(mn), inMax(mx), blendDuration(dur) {}

    void evaluate(float dt, const Skeleton& baseSkeleton, std::vector<Skeleton::Pose>& out) override;
};

// Data-driven graph. Owns all nodes, exposes single FloatParameter (speed)
// and a root pose node. Evaluate produces blended pose + final matrices via helper.
struct AnimGraph {
    FloatParameterNode speed;
    std::vector<std::unique_ptr<AnimNode>> ownedNodes;
    AnimNode* root = nullptr;
    Skeleton baseSkeleton; // template for pose sizing / bind pose

    void setSpeed(float s) { speed.value = s; }

    // Evaluate graph into pose vector (blended).
    void evaluate(float dt, std::vector<Skeleton::Pose>& outPose);

    // Convenience: evaluate and write directly into skeleton.pose + finalMatrices.
    void evaluateInto(Skeleton& skeleton, float dt);
};

// Build the example locomotion graph from spec ag09:
//          [Speed]
//             |
//        [Blend Node] (Idle -> WalkRun)
//        /          \
//   [Idle]        [Walk/Run Blend] (Walk -> Run)
// Anim indices: 0=Idle, 1=Walk, 2=Run (fallback to last available).
std::shared_ptr<AnimGraph> makeLocomotionGraph(const std::vector<Animation>& anims, const Skeleton& baseSkeleton);

} // namespace engine
