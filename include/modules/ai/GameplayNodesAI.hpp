#pragma once
#include "core/gameplay_graph.h"
#include "modules/ai/GraphContext.hpp"

#include <string>

namespace engine {

// Sensor: distance between self and target (from GraphContext)
struct DistanceNode : GameplayNode {
    float value = 0.0f;
    float getFloat() const override { return value; }
    void execute(float dt) override;
    void execute(const GraphContext& ctx, AnimParams& params) override;
    std::string typeName() const override { return "Distance"; }
    std::unique_ptr<GameplayNode> clone() const override;
};

// Logic: compare two floats
struct CompareFloatNode : GameplayNode {
    enum class Op { LESS_THAN, GREATER_THAN, LESS_EQUAL, GREATER_EQUAL, EQUAL };
    GameplayNode* a = nullptr;
    GameplayNode* b = nullptr;
    Op op = Op::LESS_THAN;
    // Threshold fallback when a/b not connected (inline editor value)
    float threshold = 5.0f;
    bool value = false;
    bool getBool() const override { return value; }
    void execute(float dt) override;
    void execute(const GraphContext& ctx, AnimParams& params) override;
    std::string typeName() const override { return "CompareFloat"; }
    std::unique_ptr<GameplayNode> clone() const override;
    static const char* opName(Op o);
    static Op opFromString(const std::string& s);
};

// Spatial action: move self towards target if enabled
struct MoveTowardsNode : GameplayNode {
    GameplayNode* speed = nullptr;
    GameplayNode* enabled = nullptr;
    float speedValue = 2.0f; // used when speed == nullptr
    bool enabledValue = true;
    void execute(float dt) override;
    void execute(const GraphContext& ctx, AnimParams& params) override;
    std::string typeName() const override { return "MoveTowards"; }
    std::unique_ptr<GameplayNode> clone() const override;
};

} // namespace engine
