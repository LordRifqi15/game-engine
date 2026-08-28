#include "core/anim_graph.h"
#include "core/animation_system.h"

#include <algorithm>
#include <cmath>

namespace engine {

void ClipNode::evaluate(float dt, const Skeleton& baseSkeleton, std::vector<Skeleton::Pose>& out) {
    time += dt * playbackSpeed;
    sampleAnimationPose(clip, time, baseSkeleton, out);
}

void BlendNode::evaluate(float dt, const Skeleton& baseSkeleton, std::vector<Skeleton::Pose>& out) {
    if (!inputA || !inputB) {
        if (inputA) inputA->evaluate(dt, baseSkeleton, out);
        else if (inputB) inputB->evaluate(dt, baseSkeleton, out);
        else out = baseSkeleton.pose;
        return;
    }
    float target = 0.0f;
    if (param) {
        float denom = inMax - inMin;
        if (std::abs(denom) > 1e-6f) target = (param->value - inMin) / denom;
        else target = param->value >= inMax ? 1.0f : 0.0f;
        target = std::clamp(target, 0.0f, 1.0f);
    }
    // Smooth toward target (prevents snap on abrupt speed change)
    if (blendDuration > 1e-6f) {
        float step = dt / blendDuration;
        if (currentAlpha < target) currentAlpha = std::min(currentAlpha + step, target);
        else if (currentAlpha > target) currentAlpha = std::max(currentAlpha - step, target);
    } else {
        currentAlpha = target;
    }

    std::vector<Skeleton::Pose> poseA, poseB;
    // Both children advance time even though we blend; each ClipNode tracks its own time.
    inputA->evaluate(dt, baseSkeleton, poseA);
    inputB->evaluate(dt, baseSkeleton, poseB);
    blendPoses(poseA, poseB, currentAlpha, out);
}

void AnimGraph::evaluate(float dt, std::vector<Skeleton::Pose>& outPose) {
    if (!root) {
        outPose = baseSkeleton.pose;
        return;
    }
    root->evaluate(dt, baseSkeleton, outPose);
}

void AnimGraph::evaluateInto(Skeleton& skeleton, float dt) {
    std::vector<Skeleton::Pose> blended;
    evaluate(dt, blended);
    if (blended.size() != skeleton.joints.size()) {
        // Ensure sizing matches skeleton (baseSkeleton is template)
        skeleton.resizePose();
        if (blended.size() < skeleton.joints.size()) blended.resize(skeleton.joints.size());
    }
    skeleton.pose = std::move(blended);
    computeFinalMatrices(skeleton);
}

std::shared_ptr<AnimGraph> makeLocomotionGraph(const std::vector<Animation>& anims, const Skeleton& baseSkeleton) {
    auto graph = std::make_shared<AnimGraph>();
    graph->baseSkeleton = baseSkeleton;
    // Ensure base pose is sized
    if (graph->baseSkeleton.pose.size() != graph->baseSkeleton.joints.size()) graph->baseSkeleton.resizePose();

    if (anims.empty()) {
        return graph; // no clips, will output bind pose
    }

    const Animation* idleAnim = &anims[0];
    const Animation* walkAnim = anims.size() > 1 ? &anims[1] : idleAnim;
    const Animation* runAnim = anims.size() > 2 ? &anims[2] : walkAnim;

    auto idleClip = std::make_unique<ClipNode>(*idleAnim);
    auto walkClip = std::make_unique<ClipNode>(*walkAnim);
    auto runClip = std::make_unique<ClipNode>(*runAnim);

    ClipNode* idlePtr = idleClip.get();
    ClipNode* walkPtr = walkClip.get();
    ClipNode* runPtr = runClip.get();

    graph->ownedNodes.push_back(std::move(idleClip));
    graph->ownedNodes.push_back(std::move(walkClip));
    graph->ownedNodes.push_back(std::move(runClip));

    // Walk/Run blend: speed 1.5 -> 2.5 maps to Walk->Run
    auto walkRunBlend = std::make_unique<BlendNode>();
    walkRunBlend->inputA = walkPtr;
    walkRunBlend->inputB = runPtr;
    walkRunBlend->param = &graph->speed;
    walkRunBlend->inMin = 1.5f;
    walkRunBlend->inMax = 2.5f;
    walkRunBlend->blendDuration = 0.3f;
    walkRunBlend->currentAlpha = 0.0f;
    BlendNode* walkRunPtr = walkRunBlend.get();
    graph->ownedNodes.push_back(std::move(walkRunBlend));

    // Final blend: Idle (0) -> Moving (WalkRun). Threshold 0.1 -> 1.0 as in previous FSM.
    auto finalBlend = std::make_unique<BlendNode>();
    finalBlend->inputA = idlePtr;
    finalBlend->inputB = walkRunPtr;
    finalBlend->param = &graph->speed;
    finalBlend->inMin = 0.1f;
    finalBlend->inMax = 1.0f;
    finalBlend->blendDuration = 0.3f;
    finalBlend->currentAlpha = 0.0f;
    BlendNode* finalPtr = finalBlend.get();
    graph->ownedNodes.push_back(std::move(finalBlend));

    graph->root = finalPtr;
    return graph;
}

} // namespace engine
