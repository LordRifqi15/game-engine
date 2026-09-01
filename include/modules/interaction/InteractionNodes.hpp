#pragma once
#include "core/gameplay_graph.h"
#include "modules/interaction/Event.hpp"
#include <string>
#include <cstdint>

namespace Engine {

enum class EmitTargetMode {
    Self,
    Target,
    Instigator
};

// Listens for incoming event by name
struct OnEventNode : public ::engine::GameplayNode {
    std::string eventName{"OnEnter"};
    // Outputs (read via getBool/getFloat and extra)
    bool triggered{false};
    float value{0.0f};
    uint32_t instigatorEntity{0};

    bool getBool() const override { return triggered; }
    float getFloat() const override { return value; }
    // Extra getter for instigator (not part of base, but for test)
    uint32_t getInstigator() const { return instigatorEntity; }

    void execute(float dt) override;
    void execute(const ::engine::GraphContext& ctx, ::engine::AnimParams& params) override;
    std::string typeName() const override { return "OnEvent"; }
    std::unique_ptr<::engine::GameplayNode> clone() const override;
};

// Emits event to target on rising edge of trigger
struct EmitEventNode : public ::engine::GameplayNode {
    ::engine::GameplayNode* triggerNode{nullptr};
    ::engine::GameplayNode* valueNode{nullptr};
    std::string eventName{"Attack"};
    EmitTargetMode targetMode{EmitTargetMode::Target};
    float value{0.0f};
    bool prevTriggered{false};

    void execute(float dt) override;
    void execute(const ::engine::GraphContext& ctx, ::engine::AnimParams& params) override;
    std::string typeName() const override { return "EmitEvent"; }
    std::unique_ptr<::engine::GameplayNode> clone() const override;
};

// Directly modifies blackboard float when event arrives
struct ModifyBlackboardOnEventNode : public ::engine::GameplayNode {
    std::string eventName{"Attack"};
    std::string targetBlackboardKey{"Health"};
    float deltaValue{-10.0f};

    void execute(float dt) override;
    void execute(const ::engine::GraphContext& ctx, ::engine::AnimParams& params) override;
    std::string typeName() const override { return "ModifyBlackboardOnEvent"; }
    std::unique_ptr<::engine::GameplayNode> clone() const override;
};

// Compatibility aliases for earlier spec naming (OnTriggerNode)
using OnTriggerNode = OnEventNode;

} // namespace Engine

namespace engine {
    using OnEventNode = ::Engine::OnEventNode;
    using EmitEventNode = ::Engine::EmitEventNode;
    using ModifyBlackboardOnEventNode = ::Engine::ModifyBlackboardOnEventNode;
    using OnTriggerNode = ::Engine::OnTriggerNode;
    using EmitTargetMode = ::Engine::EmitTargetMode;
}
