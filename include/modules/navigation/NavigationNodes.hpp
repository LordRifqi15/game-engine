#pragma once
#include "core/gameplay_graph.h"
#include "modules/ai/GraphContext.hpp"
#include <string>

namespace engine {

struct RequestPathNode : GameplayNode {
    GameplayNode* targetPosNode = nullptr; // vec3 source, if null use ctx.targetPosition
    GameplayNode* triggerNode = nullptr;   // bool
    glm::vec3 targetPos{0.0f};
    bool triggerValue{false};
    void execute(float dt) override;
    void execute(const GraphContext& ctx, AnimParams& params) override;
    std::string typeName() const override { return "RequestPath"; }
    std::unique_ptr<GameplayNode> clone() const override;
};

struct FollowPathNode : GameplayNode {
    GameplayNode* speedNode = nullptr;
    GameplayNode* enabledNode = nullptr;
    float speedValue{2.0f};
    float acceptanceRadius{0.3f};
    bool enabledValue{true};
    bool hasArrived{false};
    float currentSpeed{0.0f};
    bool getBool() const override { return hasArrived; }
    float getFloat() const override { return currentSpeed; }
    void execute(float dt) override;
    void execute(const GraphContext& ctx, AnimParams& params) override;
    std::string typeName() const override { return "FollowPath"; }
    std::unique_ptr<GameplayNode> clone() const override;
};

} // namespace engine
