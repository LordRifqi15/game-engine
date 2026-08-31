#pragma once

#include "core/anim_state_machine.h"
#include "modules/ai/GraphContext.hpp"

#include <memory>
#include <string>
#include <vector>
namespace engine {

struct EditorGraph; // forward for visual editor bridge

struct GameplayNode {
    virtual ~GameplayNode() = default;
    virtual void execute(float dt) = 0;
    // New spatial context overload for AI nodes (default forwards to dt version)
    virtual void execute(const GraphContext& ctx, AnimParams& params) {
        (void)params;
        execute(ctx.dt);
    }
    virtual std::string typeName() const = 0;
    virtual float getFloat() const { return 0.0f; }
    virtual bool getBool() const { return false; }
    virtual glm::vec3 getVec3() const { return glm::vec3(0.0f); }
    virtual std::unique_ptr<GameplayNode> clone() const = 0;
};

struct FloatNode : GameplayNode {
    float value = 0.0f;
    GameplayNode* input = nullptr;
    float getFloat() const override { return value; }
    void execute(float dt) override;
    std::string typeName() const override { return "Float"; }
    std::unique_ptr<GameplayNode> clone() const override;
};

struct KeyInputNode : GameplayNode {
    int key = 0;
    bool pressed = false;
    bool getBool() const override { return pressed; }
    void execute(float dt) override;
    std::string typeName() const override { return "KeyInput"; }
    std::unique_ptr<GameplayNode> clone() const override;
};

struct AndNode : GameplayNode {
    GameplayNode* a = nullptr;
    GameplayNode* b = nullptr;
    bool value = false;
    bool getBool() const override { return value; }
    void execute(float dt) override;
    std::string typeName() const override { return "And"; }
    std::unique_ptr<GameplayNode> clone() const override;
};

struct BranchNode : GameplayNode {
    GameplayNode* condition = nullptr;
    GameplayNode* trueValue = nullptr;
    GameplayNode* falseValue = nullptr;
    float value = 0.0f;
    float getFloat() const override { return value; }
    void execute(float dt) override;
    std::string typeName() const override { return "Branch"; }
    std::unique_ptr<GameplayNode> clone() const override;
};

struct SetFloatParamNode : GameplayNode {
    GameplayNode* input = nullptr;
    AnimParams* target = nullptr;
    float AnimParams::* member = nullptr;
    void execute(float dt) override;
    std::string typeName() const override { return "SetFloatParam"; }
    std::unique_ptr<GameplayNode> clone() const override;
};

struct SetBoolParamNode : GameplayNode {
    GameplayNode* input = nullptr;
    AnimParams* target = nullptr;
    bool AnimParams::* member = nullptr;
    bool prev = false;
    void execute(float dt) override;
    std::string typeName() const override { return "SetBoolParam"; }
    std::unique_ptr<GameplayNode> clone() const override;
};

struct TimerNode : GameplayNode {
    float value = 0.0f;
    float getFloat() const override { return value; }
    void execute(float dt) override;
    std::string typeName() const override { return "Timer"; }
    std::unique_ptr<GameplayNode> clone() const override;
private:
    float t = 0.0f;
};

struct GameplayGraph {
    std::vector<std::unique_ptr<GameplayNode>> nodes;
    AnimParams* target = nullptr;

    void setTarget(AnimParams* p);
    void execute(float dt);
    void execute(float dt, AnimParams& outParams);
    void execute(float dt, AnimParams& outParams, const GraphContext& ctx);
    void execute(const GraphContext& ctx, AnimParams& outParams);
    std::shared_ptr<GameplayGraph> clone(AnimParams* newTarget) const;

    template <typename T, typename... Args>
    T* addNode(Args&&... args) {
        auto up = std::make_unique<T>(std::forward<Args>(args)...);
        T* ptr = up.get();
        nodes.push_back(std::move(up));
        return ptr;
    }
    static std::shared_ptr<GameplayGraph> makeMinimal(AnimParams* target);
    // Task 036: autonomous NPC chase graph (Distance -> Compare -> MoveTowards + speed)
    static std::shared_ptr<GameplayGraph> makeNPCChase(AnimParams* target, float detectionRadius = 5.0f, float moveSpeed = 2.0f);
    // Task 038: blackboard chase with LastSeenPos + StateTimer (Chase->Investigate->Idle)
    static std::shared_ptr<GameplayGraph> makeBlackboardChase(AnimParams* target, float detectionRadius = 6.0f, float moveSpeed = 2.2f, float waitDuration = 3.0f);
};

// Editor -> GameplayGraph bridge (for visual editing of gameplay logic)
std::shared_ptr<GameplayGraph> buildGameplayGraph(const struct EditorGraph& ed, AnimParams* target);
struct EditorGraph makeGameplayEditorGraph();

} // namespace engine
