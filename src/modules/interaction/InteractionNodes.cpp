#include "modules/interaction/InteractionNodes.hpp"
#include "modules/ai/GraphContext.hpp"
#include "ecs/components/EventInboxComponent.hpp"
#include "ecs/components/BlackboardComponent.hpp"
#include "core/registry.h"

::engine::Registry* g_currentRegistryForEmit = nullptr;

namespace Engine {

void OnEventNode::execute(float) {
    triggered = false;
    value = 0.0f;
    instigatorEntity = 0;
}
void OnEventNode::execute(const ::engine::GraphContext& ctx, ::engine::AnimParams& /*params*/) {
    triggered = false;
    value = 0.0f;
    instigatorEntity = 0;
    if (!ctx.incomingEvents) return;
    for (auto& e : *ctx.incomingEvents) {
        if (e.name == eventName) {
            triggered = true;
            value = e.value;
            instigatorEntity = e.instigatorEntity;
            break;
        }
    }
}
std::unique_ptr<::engine::GameplayNode> OnEventNode::clone() const {
    auto up = std::make_unique<OnEventNode>();
    up->eventName = eventName;
    up->triggered = triggered;
    up->value = value;
    up->instigatorEntity = instigatorEntity;
    return up;
}

void EmitEventNode::execute(float) {
    bool cur = triggerNode ? triggerNode->getBool() : false;
    prevTriggered = cur;
}
void EmitEventNode::execute(const ::engine::GraphContext& ctx, ::engine::AnimParams& /*params*/) {
    bool curTriggered = triggerNode ? triggerNode->getBool() : false;
    float curValue = valueNode ? valueNode->getFloat() : value;
    bool rising = curTriggered && !prevTriggered;
    prevTriggered = curTriggered;
    if (!rising) return;
    uint32_t targetId = 0;
    switch (targetMode) {
        case EmitTargetMode::Self: targetId = ctx.selfEntity; break;
        case EmitTargetMode::Target: targetId = ctx.targetEntity; break;
        case EmitTargetMode::Instigator: {
            if (ctx.incomingEvents && !ctx.incomingEvents->empty()) targetId = ctx.incomingEvents->front().instigatorEntity;
            else targetId = ctx.selfEntity;
            break;
        }
    }
    if (targetId == ::engine::kInvalidEntity) return;
    ::engine::Registry* reg = ctx.registry ? ctx.registry : g_currentRegistryForEmit;
    if (!reg) return;
    if (!reg->valid(static_cast<entt::entity>(targetId))) return;
    EventInboxComponent* inbox = nullptr;
    if (reg->all_of<EventInboxComponent>(static_cast<entt::entity>(targetId))) {
        inbox = &reg->get<EventInboxComponent>(static_cast<entt::entity>(targetId));
    } else {
        auto& nb = reg->emplace<EventInboxComponent>(static_cast<entt::entity>(targetId));
        inbox = &nb;
    }
    inbox->post(Event{eventName, ctx.selfEntity, targetId, curValue});
}
std::unique_ptr<::engine::GameplayNode> EmitEventNode::clone() const {
    auto up = std::make_unique<EmitEventNode>();
    up->triggerNode = triggerNode;
    up->valueNode = valueNode;
    up->eventName = eventName;
    up->targetMode = targetMode;
    up->value = value;
    up->prevTriggered = prevTriggered;
    return up;
}

void ModifyBlackboardOnEventNode::execute(float) {}
void ModifyBlackboardOnEventNode::execute(const ::engine::GraphContext& ctx, ::engine::AnimParams& /*params*/) {
    if (!ctx.incomingEvents || !ctx.blackboard) return;
    for (auto& e : *ctx.incomingEvents) {
        if (e.name == eventName) {
            float cur = ctx.blackboard->getFloat(targetBlackboardKey, 0.0f);
            ctx.blackboard->setFloat(targetBlackboardKey, cur + deltaValue);
            break;
        }
    }
}
std::unique_ptr<::engine::GameplayNode> ModifyBlackboardOnEventNode::clone() const {
    auto up = std::make_unique<ModifyBlackboardOnEventNode>();
    up->eventName = eventName;
    up->targetBlackboardKey = targetBlackboardKey;
    up->deltaValue = deltaValue;
    return up;
}

} // namespace Engine
