#pragma once

#include "core/skeleton.h"

namespace engine {

// Evaluate animation at time (seconds) and update skeleton pose.
// Time is expected to be already wrapped to [0, duration) if looping.
void updateSkeletonFromAnimation(Skeleton& skeleton, const Animation& anim, float time);

// Compute final joint matrices: global * inverseBind for all joints.
// Assumes skeleton.pose already contains local TRS.
void computeFinalMatrices(Skeleton& skeleton);

// Task 030: blending (kept for graph reuse)
void sampleAnimationPose(const Animation& anim, float time, const Skeleton& baseSkeleton, std::vector<Skeleton::Pose>& outPose);
void blendPoses(const std::vector<Skeleton::Pose>& a, const std::vector<Skeleton::Pose>& b, float t, std::vector<Skeleton::Pose>& out);
void updateAnimationComponent(AnimationComponent& comp, Skeleton& skeleton, float dt);

} // namespace engine
