#include "modules/ai/GameplayNodesAI.hpp"

#include "ecs/components/PhysicsComponent.hpp"
#include <glm/gtc/quaternion.hpp>
#include <cmath>
namespace engine {

void DistanceNode::execute(float) {
    // No context -> cannot compute, keep 0
}
void DistanceNode::execute(const GraphContext& ctx, AnimParams& /*params*/) {
    value = glm::distance(ctx.selfPosition, ctx.targetPosition);
}
std::unique_ptr<GameplayNode> DistanceNode::clone() const {
    auto up = std::make_unique<DistanceNode>();
    up->value = value;
    return up;
}

void CompareFloatNode::execute(float) {
    // Without context, evaluate using current stored value if inputs missing
    float av = a ? a->getFloat() : threshold;
    float bv = b ? b->getFloat() : threshold;
    switch (op) {
        case Op::LESS_THAN: value = av < bv; break;
        case Op::GREATER_THAN: value = av > bv; break;
        case Op::LESS_EQUAL: value = av <= bv; break;
        case Op::GREATER_EQUAL: value = av >= bv; break;
        case Op::EQUAL: value = std::fabs(av - bv) < 1e-5f; break;
    }
}
void CompareFloatNode::execute(const GraphContext& ctx, AnimParams& params) {
    // Reuse same logic but ensure inputs already executed this frame (graph ensures order)
    (void)ctx; (void)params;
    execute(0.0f);
}
std::unique_ptr<GameplayNode> CompareFloatNode::clone() const {
    auto up = std::make_unique<CompareFloatNode>();
    up->a = a;
    up->b = b;
    up->op = op;
    up->threshold = threshold;
    up->value = value;
    return up;
}
const char* CompareFloatNode::opName(Op o) {
    switch (o) {
        case Op::LESS_THAN: return "LESS_THAN";
        case Op::GREATER_THAN: return "GREATER_THAN";
        case Op::LESS_EQUAL: return "LESS_EQUAL";
        case Op::GREATER_EQUAL: return "GREATER_EQUAL";
        case Op::EQUAL: return "EQUAL";
    }
    return "LESS_THAN";
}
CompareFloatNode::Op CompareFloatNode::opFromString(const std::string& s) {
    if (s == "GREATER_THAN") return Op::GREATER_THAN;
    if (s == "LESS_EQUAL") return Op::LESS_EQUAL;
    if (s == "GREATER_EQUAL") return Op::GREATER_EQUAL;
    if (s == "EQUAL") return Op::EQUAL;
    return Op::LESS_THAN;
}

void MoveTowardsNode::execute(float) {
    // No context -> no-op
}
void MoveTowardsNode::execute(const GraphContext& ctx, AnimParams& /*params*/) {
    bool en = enabled ? enabled->getBool() : enabledValue;
    if (!en) return;
    glm::vec3 target = ctx.targetPosition;
    if (ctx.blackboard) {
        if (ctx.blackboard->getBool("IsSearching", false)) {
            glm::vec3 lastSeen = ctx.blackboard->getVec3("LastSeenPos", target);
            // Use LastSeenPos if it was set (non-zero or hasKey)
            if (ctx.blackboard->hasKey("LastSeenPos")) target = lastSeen;
        }
    }
    // Prefer physics velocity path (Task 037), fallback to legacy direct position
    if (ctx.outPhysics) {
        float dist = glm::distance(ctx.selfPosition, target);
        if (dist <= 0.05f) return;
        if (dist < 0.001f) return;
        float spd = speed ? speed->getFloat() : speedValue;
        glm::vec3 dir = glm::normalize(target - ctx.selfPosition);
        ctx.outPhysics->velocity.x = dir.x * spd;
        ctx.outPhysics->velocity.z = dir.z * spd;
        float yaw = std::atan2(dir.x, dir.z);
        if (ctx.outSelfRotation) *ctx.outSelfRotation = glm::angleAxis(yaw, glm::vec3(0, 1, 0));
        if (ctx.outSelfRotationEuler) ctx.outSelfRotationEuler->y = yaw;
        return;
    }
    if (!ctx.outSelfPosition) return;
    float dist = glm::distance(*ctx.outSelfPosition, target);
    if (dist <= 0.05f) return;
    if (dist < 0.001f) return;
    float spd = speed ? speed->getFloat() : speedValue;
    glm::vec3 dir = glm::normalize(target - *ctx.outSelfPosition);
    *ctx.outSelfPosition += dir * spd * ctx.dt;
    float yaw = std::atan2(dir.x, dir.z);
    if (ctx.outSelfRotation) *ctx.outSelfRotation = glm::angleAxis(yaw, glm::vec3(0, 1, 0));
    if (ctx.outSelfRotationEuler) ctx.outSelfRotationEuler->y = yaw;
}
std::unique_ptr<GameplayNode> MoveTowardsNode::clone() const {
    auto up = std::make_unique<MoveTowardsNode>();
    up->speed = speed;
    up->enabled = enabled;
    up->speedValue = speedValue;
    up->enabledValue = enabledValue;
    return up;
}

void ApplyImpulseNode::execute(float) {}
void ApplyImpulseNode::execute(const GraphContext& ctx, AnimParams& /*params*/) {
    bool trig = trigger ? trigger->getBool() : false;
    bool rising = trig && !prevTrigger;
    prevTrigger = trig;
    if (!rising) return;
    if (!ctx.outPhysics) return;
    if (!ctx.outPhysics->isGrounded) return;
    float impY = impulseNode ? impulseNode->getFloat() : impulse.y;
    // If impulseNode provides float, use it as Y impulse; otherwise use stored vector's Y
    if (impulseNode) impY = impulseNode->getFloat();
    else impY = impulse.y;
    float mass = ctx.outPhysics->mass > 0.0f ? ctx.outPhysics->mass : 1.0f;
    ctx.outPhysics->velocity.y += impY / mass;
}
std::unique_ptr<GameplayNode> ApplyImpulseNode::clone() const {
    auto up = std::make_unique<ApplyImpulseNode>();
    up->impulseNode = impulseNode;
    up->trigger = trigger;
    up->impulse = impulse;
    up->prevTrigger = false; // fresh state per clone
    return up;
}


} // namespace engine
