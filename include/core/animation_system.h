#pragma once

#include "core/skeleton.h"

namespace engine {

// Compute final joint matrices: global * inverseBind for all joints.
// Assumes skeleton.pose already contains local TRS.
void computeFinalMatrices(Skeleton& skeleton);

// Pose sampling + blending primitives (reused by AnimGraph, Task 029).
void sampleAnimationPose(const Animation& anim, float time, const Skeleton& baseSkeleton, std::vector<Skeleton::Pose>& outPose);
void blendPoses(const std::vector<Skeleton::Pose>& a, const std::vector<Skeleton::Pose>& b, float t, std::vector<Skeleton::Pose>& out);

} // namespace engine
