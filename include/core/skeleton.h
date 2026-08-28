#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

namespace engine {

// Max joints per skeleton (SSBO size, keep under UBO limit if used).
constexpr uint32_t kMaxJoints = 128;

// One joint in a skeleton hierarchy.
struct Joint {
    glm::mat4 inverseBind{1.0f};
    int parent = -1; // joint index, -1 = root
    std::string name;
};

// Skeleton: hierarchy of joints + per-frame computed matrices.
struct Skeleton {
    std::vector<Joint> joints;

    // Local TRS per joint (updated by animation), then global = parentGlobal * local.
    struct Pose {
        glm::vec3 translation{0.0f};
        glm::vec3 scale{1.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // w,x,y,z (glm quat)
    };
    std::vector<Pose> pose; // size == joints.size()

    // Final matrices uploaded to GPU: global * inverseBind
    std::vector<glm::mat4> finalMatrices;

    // Helper: ensure pose/final sized to joints.
    void resizePose() {
        pose.resize(joints.size());
        finalMatrices.resize(joints.size(), glm::mat4(1.0f));
    }
};

// Animation sampler: keyframe times + output values.
// For translation/scale: outputs are vec3 stored as vec4 (w unused).
// For rotation: outputs are quat vec4 (x,y,z,w) with w last as per glTF.
struct AnimationSampler {
    std::string interpolation = "LINEAR"; // LINEAR, STEP, CUBICSPLINE
    std::vector<float> inputs;            // time stamps
    std::vector<glm::vec4> outputs;       // values
};

struct AnimationChannel {
    int targetJoint = -1;      // joint index to animate, -1 = not a joint
    std::string path;          // translation, rotation, scale
    int samplerIndex = -1;
};

struct Animation {
    std::string name;
    float duration = 0.0f;
    std::vector<AnimationSampler> samplers;
    std::vector<AnimationChannel> channels;
};

struct AnimState {
    int currentAnim = 0;
    int nextAnim = -1;
    float time = 0.0f;
    float blendTime = 0.0f;
    float blendDuration = 0.25f;
};

enum class LocomotionState { Idle, Walk, Run };

// Components for registry (ECS)
struct SkeletonComponent {
    Skeleton skeleton;
};

struct AnimationComponent {
    std::vector<Animation> animations;
    AnimState state;
    float speed = 1.0f;
    bool playing = true;
    bool loop = true;
    LocomotionState playState = LocomotionState::Idle;

    void crossFadeTo(int next, float duration) {
        if (next < 0 || next >= (int)animations.size()) return;
        if (next == state.currentAnim && state.nextAnim == -1) return;
        if (state.nextAnim == next) return;
        state.nextAnim = next;
        state.blendTime = 0.0f;
        state.blendDuration = duration > 0.0f ? duration : 0.25f;
    }
    bool isBlending() const { return state.nextAnim != -1; }
};




} // namespace engine
