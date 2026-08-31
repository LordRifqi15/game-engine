#include "modules/navigation/NavigationNodes.hpp"
#include "modules/navigation/Pathfinding.hpp"
#include "ecs/components/PathComponent.hpp"
#include <glm/gtc/quaternion.hpp>
#include <cmath>

namespace engine {

void RequestPathNode::execute(float) {}
void RequestPathNode::execute(const GraphContext& ctx, AnimParams& /*params*/) {
    if (!ctx.path || !ctx.navGrid) return;
    bool trig = triggerNode ? triggerNode->getBool() : triggerValue;
    if (!trig) return;
    glm::vec3 target = targetPosNode ? targetPosNode->getVec3() : targetPos;
    // Use targetPos if targetPosNode is null and targetPos is zero, fallback to ctx.targetPosition
    if (!targetPosNode && targetPos == glm::vec3(0.0f)) {
        target = ctx.targetPosition;
    } else if (targetPosNode) {
        target = targetPosNode->getVec3();
    } else {
        target = targetPos;
        if (target == glm::vec3(0.0f)) target = ctx.targetPosition;
    }
    float distToDest = glm::distance(target, ctx.path->destination);
    if (ctx.path->hasPath && distToDest <= 0.5f) return;
    auto waypoints = Pathfinding::findPath(*ctx.navGrid, ctx.selfPosition, target);
    if (waypoints.empty()) {
        ctx.path->hasPath = false;
        ctx.path->isFinished = false;
        return;
    }
    ctx.path->waypoints = std::move(waypoints);
    ctx.path->currentIndex = 0;
    ctx.path->hasPath = true;
    ctx.path->isFinished = false;
    ctx.path->destination = target;
}
std::unique_ptr<GameplayNode> RequestPathNode::clone() const {
    auto up = std::make_unique<RequestPathNode>();
    up->targetPosNode = targetPosNode;
    up->triggerNode = triggerNode;
    up->targetPos = targetPos;
    up->triggerValue = triggerValue;
    return up;
}

void FollowPathNode::execute(float) {
    // No context -> no-op, keep hasArrived as is
}
void FollowPathNode::execute(const GraphContext& ctx, AnimParams& /*params*/) {
    if (!ctx.path || !ctx.outPhysics) {
        // Even without physics, update hasArrived from path state
        if (ctx.path) {
            hasArrived = ctx.path->isFinished;
            currentSpeed = 0.0f;
        }
        return;
    }
    bool en = enabledNode ? enabledNode->getBool() : enabledValue;
    if (!en || !ctx.path->hasPath || ctx.path->isFinished) {
        hasArrived = ctx.path ? ctx.path->isFinished : false;
        currentSpeed = 0.0f;
        // Allow damping to stop (do not zero velocity here, physics will damp)
        return;
    }
    if (ctx.path->currentIndex >= ctx.path->waypoints.size()) {
        ctx.path->isFinished = true;
        ctx.path->hasPath = false;
        hasArrived = true;
        currentSpeed = 0.0f;
        return;
    }
    glm::vec3 targetWp = ctx.path->waypoints[ctx.path->currentIndex];
    // Horizontal distance only (XZ)
    glm::vec2 selfXZ(ctx.selfPosition.x, ctx.selfPosition.z);
    glm::vec2 wpXZ(targetWp.x, targetWp.z);
    float dist = glm::distance(selfXZ, wpXZ);
    if (dist <= acceptanceRadius) {
        ctx.path->currentIndex++;
        if (ctx.path->currentIndex >= ctx.path->waypoints.size()) {
            ctx.path->isFinished = true;
            ctx.path->hasPath = false;
            hasArrived = true;
            currentSpeed = 0.0f;
            return;
        }
        targetWp = ctx.path->waypoints[ctx.path->currentIndex];
        dist = glm::distance(selfXZ, glm::vec2(targetWp.x, targetWp.z));
    }
    float spd = speedNode ? speedNode->getFloat() : speedValue;
    currentSpeed = spd;
    hasArrived = false;
    // Steering: normalize horizontal dir, keep y from physics (gravity)
    glm::vec3 dir = targetWp - ctx.selfPosition;
    dir.y = 0.0f;
    float len = glm::length(glm::vec2(dir.x, dir.z));
    if (len < 0.001f) return;
    dir = glm::normalize(dir);
    ctx.outPhysics->velocity.x = dir.x * spd;
    ctx.outPhysics->velocity.z = dir.z * spd;
    // Orient
    float yaw = std::atan2(dir.x, dir.z);
    if (ctx.outSelfRotation) *ctx.outSelfRotation = glm::angleAxis(yaw, glm::vec3(0,1,0));
    if (ctx.outSelfRotationEuler) ctx.outSelfRotationEuler->y = yaw;
}
std::unique_ptr<GameplayNode> FollowPathNode::clone() const {
    auto up = std::make_unique<FollowPathNode>();
    up->speedNode = speedNode;
    up->enabledNode = enabledNode;
    up->speedValue = speedValue;
    up->acceptanceRadius = acceptanceRadius;
    up->enabledValue = enabledValue;
    up->hasArrived = false;
    up->currentSpeed = 0.0f;
    return up;
}

} // namespace engine
