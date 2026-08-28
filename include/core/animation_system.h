#pragma once

#include "core/skeleton.h"

namespace engine {

// Evaluate animation at time (seconds) and update skeleton pose.
// Time is expected to be already wrapped to [0, duration) if looping.
void updateSkeletonFromAnimation(Skeleton& skeleton, const Animation& anim, float time);

// Compute final joint matrices: global * inverseBind for all joints.
// Assumes skeleton.pose already contains local TRS.
void computeFinalMatrices(Skeleton& skeleton);

} // namespace engine
