#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <memory>
#include <string>
#include <vector>

namespace engine {

struct AnimGraph; // forward for data-driven graph (Task 029)
struct AnimStateMachine; // forward for state machine (Task 030)

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

// Components for registry (ECS)
struct SkeletonComponent {
    Skeleton skeleton;
};

struct AnimationComponent {
    std::vector<Animation> animations;
    float speed = 1.0f;
    bool playing = true;
    bool loop = true;
    std::shared_ptr<AnimStateMachine> machine; // Task 030: state machine -> graphs -> pose
};

} // namespace engine
