#include "modules/ai/BlackboardNodes.hpp"
#include "ecs/components/BlackboardComponent.hpp"

namespace engine {

void SetBlackboardVec3Node::execute(float) {}
void SetBlackboardVec3Node::execute(const GraphContext& ctx, AnimParams& /*params*/) {
    if (!ctx.blackboard) return;
    bool cond = condition ? condition->getBool() : true;
    if (!cond) return;
    glm::vec3 val = value ? value->getVec3() : defaultValue;
    // Special: if value is nullptr and we want to store targetPosition directly, check key == "LastSeenPos" fallback?
    // For generic, use value node's vec3; if value is nullptr and key is LastSeenPos, store targetPosition
    if (!value && key == "LastSeenPos") {
        val = ctx.targetPosition;
    }
    ctx.blackboard->setVec3(key, val);
}
std::unique_ptr<GameplayNode> SetBlackboardVec3Node::clone() const {
    auto up = std::make_unique<SetBlackboardVec3Node>();
    up->key = key;
    up->value = value;
    up->condition = condition;
    up->defaultValue = defaultValue;
    return up;
}

void SetBlackboardFloatNode::execute(float) {}
void SetBlackboardFloatNode::execute(const GraphContext& ctx, AnimParams& /*params*/) {
    if (!ctx.blackboard) return;
    bool cond = condition ? condition->getBool() : true;
    if (!cond) return;
    float val = value ? value->getFloat() : defaultValue;
    ctx.blackboard->setFloat(key, val);
}
std::unique_ptr<GameplayNode> SetBlackboardFloatNode::clone() const {
    auto up = std::make_unique<SetBlackboardFloatNode>();
    up->key = key;
    up->value = value;
    up->condition = condition;
    up->defaultValue = defaultValue;
    return up;
}

void SetBlackboardBoolNode::execute(float) {}
void SetBlackboardBoolNode::execute(const GraphContext& ctx, AnimParams& /*params*/) {
    if (!ctx.blackboard) return;
    bool cond = condition ? condition->getBool() : true;
    if (!cond) return;
    bool val = value ? value->getBool() : defaultValue;
    ctx.blackboard->setBool(key, val);
}
std::unique_ptr<GameplayNode> SetBlackboardBoolNode::clone() const {
    auto up = std::make_unique<SetBlackboardBoolNode>();
    up->key = key;
    up->value = value;
    up->condition = condition;
    up->defaultValue = defaultValue;
    return up;
}

void GetBlackboardVec3Node::execute(float) {}
void GetBlackboardVec3Node::execute(const GraphContext& ctx, AnimParams& /*params*/) {
    if (!ctx.blackboard) { cached = defaultValue; return; }
    cached = ctx.blackboard->getVec3(key, defaultValue);
}
std::unique_ptr<GameplayNode> GetBlackboardVec3Node::clone() const {
    auto up = std::make_unique<GetBlackboardVec3Node>();
    up->key = key;
    up->defaultValue = defaultValue;
    up->cached = cached;
    return up;
}

void GetBlackboardFloatNode::execute(float) {}
void GetBlackboardFloatNode::execute(const GraphContext& ctx, AnimParams& /*params*/) {
    if (!ctx.blackboard) { cached = defaultValue; return; }
    cached = ctx.blackboard->getFloat(key, defaultValue);
}
std::unique_ptr<GameplayNode> GetBlackboardFloatNode::clone() const {
    auto up = std::make_unique<GetBlackboardFloatNode>();
    up->key = key;
    up->defaultValue = defaultValue;
    up->cached = cached;
    return up;
}

void GetBlackboardBoolNode::execute(float) {}
void GetBlackboardBoolNode::execute(const GraphContext& ctx, AnimParams& /*params*/) {
    if (!ctx.blackboard) { cached = defaultValue; return; }
    cached = ctx.blackboard->getBool(key, defaultValue);
}
std::unique_ptr<GameplayNode> GetBlackboardBoolNode::clone() const {
    auto up = std::make_unique<GetBlackboardBoolNode>();
    up->key = key;
    up->defaultValue = defaultValue;
    up->cached = cached;
    return up;
}

void StateTimerNode::execute(float dt) {
    bool shouldReset = reset ? reset->getBool() : false;
    if (shouldReset) {
        elapsed = 0.0f;
        isFinished = false;
        return;
    }
    float dur = durationNode ? durationNode->getFloat() : duration;
    elapsed += dt;
    isFinished = elapsed >= dur;
}
void StateTimerNode::execute(const GraphContext& ctx, AnimParams& /*params*/) {
    bool shouldReset = reset ? reset->getBool() : false;
    if (shouldReset) {
        elapsed = 0.0f;
        isFinished = false;
        return;
    }
    float dur = durationNode ? durationNode->getFloat() : duration;
    elapsed += ctx.dt;
    isFinished = elapsed >= dur;
}
std::unique_ptr<GameplayNode> StateTimerNode::clone() const {
    auto up = std::make_unique<StateTimerNode>();
    up->reset = reset;
    up->durationNode = durationNode;
    up->duration = duration;
    up->elapsed = 0.0f; // fresh per clone, not copying elapsed
    up->isFinished = false;
    return up;
}

} // namespace engine
