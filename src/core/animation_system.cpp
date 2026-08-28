#include "core/animation_system.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace engine {

namespace {


glm::vec3 lerpVec3(const glm::vec3& a, const glm::vec3& b, float t) {
    return a + t * (b - a);
}

int findKeyframe(const std::vector<float>& times, float t) {
    if (times.empty()) return -1;
    if (t <= times.front()) return 0;
    if (t >= times.back()) return (int)times.size() - 1;
    // binary search for lower bound
    int lo = 0, hi = (int)times.size() - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (times[mid] <= t && t < times[mid + 1]) return mid;
        if (times[mid] < t) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;
}

glm::vec4 interpolateVec4(const AnimationSampler& sampler, float time) {
    if (sampler.inputs.empty() || sampler.outputs.empty()) return glm::vec4(0.0f);
    if (sampler.inputs.size() == 1) return sampler.outputs[0];
    // Wrap time if needed (should be pre-wrapped)
    int idx = findKeyframe(sampler.inputs, time);
    if (idx < 0) return sampler.outputs[0];
    if (idx >= (int)sampler.inputs.size() - 1) return sampler.outputs.back();
    if (sampler.interpolation == "STEP") {
        return sampler.outputs[idx];
    }
    // LINEAR (and CUBICSPLINE fallback to linear)
    float t0 = sampler.inputs[idx];
    float t1 = sampler.inputs[idx + 1];
    float factor = (t1 > t0) ? (time - t0) / (t1 - t0) : 0.0f;
    factor = std::clamp(factor, 0.0f, 1.0f);
    const glm::vec4& a = sampler.outputs[idx];
    const glm::vec4& b = sampler.outputs[idx + 1];
    if (sampler.interpolation == "CUBICSPLINE") {
        // glTF CUBICSPLINE has 3 values per key: inTangent, value, outTangent
        // For simplicity, fallback to linear using the middle values.
        // Proper would be hermite, but linear is acceptable for demo.
    }
    // For vec3 data, w is unused, lerp xyz
    return a + factor * (b - a);
}

} // namespace

void updateSkeletonFromAnimation(Skeleton& skeleton, const Animation& anim, float time) {
    if (skeleton.joints.empty() || anim.channels.empty()) return;
    // Ensure pose sized
    if (skeleton.pose.size() != skeleton.joints.size()) skeleton.resizePose();

    // For looping, wrap time
    float t = time;
    if (anim.duration > 0.0f) {
        t = std::fmod(t, anim.duration);
        if (t < 0) t += anim.duration;
    }

    for (const auto& channel : anim.channels) {
        if (channel.targetJoint < 0 || channel.targetJoint >= (int)skeleton.joints.size()) continue;
        if (channel.samplerIndex < 0 || channel.samplerIndex >= (int)anim.samplers.size()) continue;
        const auto& sampler = anim.samplers[channel.samplerIndex];
        glm::vec4 value = interpolateVec4(sampler, t);
        auto& pose = skeleton.pose[channel.targetJoint];
        if (channel.path == "translation") {
            pose.translation = glm::vec3(value);
        } else if (channel.path == "scale") {
            pose.scale = glm::vec3(value);
        } else if (channel.path == "rotation") {
            // glTF rotation is xyzw, glm quat is w,x,y,z
            glm::quat q(value.w, value.x, value.y, value.z);
            pose.rotation = glm::normalize(q);
        }
    }
}

void computeFinalMatrices(Skeleton& skeleton) {
    if (skeleton.joints.empty()) return;
    if (skeleton.pose.size() != skeleton.joints.size()) skeleton.resizePose();
    size_t n = skeleton.joints.size();
    std::vector<glm::mat4> globals(n, glm::mat4(1.0f));
    std::vector<bool> computed(n, false);

    // Recursive helper
    std::function<glm::mat4(int)> getGlobal = [&](int idx) -> glm::mat4 {
        if (computed[idx]) return globals[idx];
        const auto& joint = skeleton.joints[idx];
        const auto& p = skeleton.pose[idx];
        glm::mat4 local = glm::translate(glm::mat4(1.0f), p.translation) *
                          glm::mat4_cast(p.rotation) *
                          glm::scale(glm::mat4(1.0f), p.scale);
        glm::mat4 global;
        if (joint.parent >= 0) {
            global = getGlobal(joint.parent) * local;
        } else {
            global = local;
        }
        globals[idx] = global;
        computed[idx] = true;
        return global;
    };

    for (size_t i = 0; i < n; ++i) {
        glm::mat4 g = getGlobal((int)i);
        skeleton.finalMatrices[i] = g * skeleton.joints[i].inverseBind;
    }
}

void sampleAnimationPose(const Animation& anim, float time, const Skeleton& baseSkeleton, std::vector<Skeleton::Pose>& outPose) {
    outPose.resize(baseSkeleton.joints.size());
    // init from base pose (or identity)
    for (size_t i = 0; i < baseSkeleton.joints.size(); ++i) {
        if (i < baseSkeleton.pose.size()) outPose[i] = baseSkeleton.pose[i];
        else outPose[i] = Skeleton::Pose{};
    }
    if (baseSkeleton.joints.empty() || anim.channels.empty()) return;
    float t = time;
    if (anim.duration > 0.0f) {
        t = std::fmod(t, anim.duration);
        if (t < 0) t += anim.duration;
    }
    for (const auto& channel : anim.channels) {
        if (channel.targetJoint < 0 || channel.targetJoint >= (int)outPose.size()) continue;
        if (channel.samplerIndex < 0 || channel.samplerIndex >= (int)anim.samplers.size()) continue;
        const auto& sampler = anim.samplers[channel.samplerIndex];
        glm::vec4 value = interpolateVec4(sampler, t);
        auto& pose = outPose[channel.targetJoint];
        if (channel.path == "translation") pose.translation = glm::vec3(value);
        else if (channel.path == "scale") pose.scale = glm::vec3(value);
        else if (channel.path == "rotation") {
            glm::quat q(value.w, value.x, value.y, value.z);
            pose.rotation = glm::normalize(q);
        }
    }
}

void blendPoses(const std::vector<Skeleton::Pose>& a, const std::vector<Skeleton::Pose>& b, float t, std::vector<Skeleton::Pose>& out) {
    size_t n = std::max(a.size(), b.size());
    out.resize(n);
    float clamped = std::clamp(t, 0.0f, 1.0f);
    for (size_t i = 0; i < n; ++i) {
        const auto& pa = i < a.size() ? a[i] : Skeleton::Pose{};
        const auto& pb = i < b.size() ? b[i] : pa;
        auto& o = out[i];
        o.translation = pa.translation + clamped * (pb.translation - pa.translation);
        o.scale = pa.scale + clamped * (pb.scale - pa.scale);
        o.rotation = glm::slerp(pa.rotation, pb.rotation, clamped);
        o.rotation = glm::normalize(o.rotation);
    }
}

void updateAnimationComponent(AnimationComponent& comp, Skeleton& skeleton, float dt) {
    if (comp.animations.empty() || !comp.playing || skeleton.joints.empty()) return;
    if (skeleton.pose.size() != skeleton.joints.size()) skeleton.resizePose();
    // clamp indices
    if (comp.state.currentAnim < 0 || comp.state.currentAnim >= (int)comp.animations.size()) comp.state.currentAnim = 0;
    if (comp.state.nextAnim >= (int)comp.animations.size()) comp.state.nextAnim = -1;

    comp.state.time += dt * comp.speed;

    if (comp.state.nextAnim != -1) {
        // blending
        comp.state.blendTime += dt;
        float blendFactor = comp.state.blendDuration > 0.0f ? comp.state.blendTime / comp.state.blendDuration : 1.0f;
        blendFactor = std::clamp(blendFactor, 0.0f, 1.0f);

        std::vector<Skeleton::Pose> poseA, poseB, blended;
        sampleAnimationPose(comp.animations[comp.state.currentAnim], comp.state.time, skeleton, poseA);
        sampleAnimationPose(comp.animations[comp.state.nextAnim], comp.state.time, skeleton, poseB);
        blendPoses(poseA, poseB, blendFactor, blended);
        skeleton.pose = std::move(blended);
        computeFinalMatrices(skeleton);

        if (comp.state.blendTime >= comp.state.blendDuration) {
            comp.state.currentAnim = comp.state.nextAnim;
            comp.state.nextAnim = -1;
            comp.state.blendTime = 0.0f;
            comp.state.blendDuration = 0.25f;
        }
    } else {
        updateSkeletonFromAnimation(skeleton, comp.animations[comp.state.currentAnim], comp.state.time);
        computeFinalMatrices(skeleton);
    }
}

} // namespace engine
