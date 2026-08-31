#pragma once
#include "core/gameplay_graph.h"
#include "modules/ai/GraphContext.hpp"
#include <string>

namespace engine {

// Set nodes: write to blackboard when condition true (or unconnected)
struct SetBlackboardVec3Node : GameplayNode {
    std::string key;
    GameplayNode* value = nullptr;
    GameplayNode* condition = nullptr;
    glm::vec3 defaultValue{0.0f};
    void execute(float dt) override;
    void execute(const GraphContext& ctx, AnimParams& params) override;
    std::string typeName() const override { return "SetBlackboardVec3"; }
    std::unique_ptr<GameplayNode> clone() const override;
};

struct SetBlackboardFloatNode : GameplayNode {
    std::string key;
    GameplayNode* value = nullptr;
    GameplayNode* condition = nullptr;
    float defaultValue = 0.0f;
    void execute(float dt) override;
    void execute(const GraphContext& ctx, AnimParams& params) override;
    std::string typeName() const override { return "SetBlackboardFloat"; }
    std::unique_ptr<GameplayNode> clone() const override;
};

struct SetBlackboardBoolNode : GameplayNode {
    std::string key;
    GameplayNode* value = nullptr;
    GameplayNode* condition = nullptr;
    bool defaultValue = false;
    void execute(float dt) override;
    void execute(const GraphContext& ctx, AnimParams& params) override;
    std::string typeName() const override { return "SetBlackboardBool"; }
    std::unique_ptr<GameplayNode> clone() const override;
};

// Get nodes: read from blackboard with fallback
struct GetBlackboardVec3Node : GameplayNode {
    std::string key;
    glm::vec3 defaultValue{0.0f};
    glm::vec3 cached{0.0f};
    glm::vec3 getVec3() const override { return cached; }
    void execute(float dt) override;
    void execute(const GraphContext& ctx, AnimParams& params) override;
    std::string typeName() const override { return "GetBlackboardVec3"; }
    std::unique_ptr<GameplayNode> clone() const override;
};

struct GetBlackboardFloatNode : GameplayNode {
    std::string key;
    float defaultValue = 0.0f;
    float cached = 0.0f;
    float getFloat() const override { return cached; }
    void execute(float dt) override;
    void execute(const GraphContext& ctx, AnimParams& params) override;
    std::string typeName() const override { return "GetBlackboardFloat"; }
    std::unique_ptr<GameplayNode> clone() const override;
};

struct GetBlackboardBoolNode : GameplayNode {
    std::string key;
    bool defaultValue = false;
    bool cached = false;
    bool getBool() const override { return cached; }
    void execute(float dt) override;
    void execute(const GraphContext& ctx, AnimParams& params) override;
    std::string typeName() const override { return "GetBlackboardBool"; }
    std::unique_ptr<GameplayNode> clone() const override;
};

// State timer: accumulates dt, reset on true, outputs elapsed and isFinished
struct StateTimerNode : GameplayNode {
    GameplayNode* reset = nullptr;
    GameplayNode* durationNode = nullptr;
    float duration = 3.0f;
    float elapsed = 0.0f;
    bool isFinished = false;
    float getFloat() const override { return elapsed; }
    bool getBool() const override { return isFinished; }
    void execute(float dt) override;
    void execute(const GraphContext& ctx, AnimParams& params) override;
    std::string typeName() const override { return "StateTimer"; }
    std::unique_ptr<GameplayNode> clone() const override;
};

} // namespace engine
