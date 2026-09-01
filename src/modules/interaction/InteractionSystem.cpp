#include "modules/interaction/InteractionSystem.hpp"
#include "core/transform_component.h"
#include "ecs/components/TriggerComponent.hpp"
#include "ecs/components/TagComponent.hpp"
#include "ecs/components/EventInboxComponent.hpp"

#include <glm/glm.hpp>
#include <unordered_set>

namespace Engine {

void InteractionSystem::update(entt::registry& registry) {
    // 1. Clear previous frame's events in all inboxes
    // Use getAllEntities + hasComponent to find inboxes (since view is single-component)
    {
        auto all = registry.getAllEntities();
        for (auto e : all) {
            if (registry.all_of<EventInboxComponent>(e)) {
                registry.get<EventInboxComponent>(e).clear();
            }
        }
    }

    // 2. Query proximity triggers - iterate over all entities with Trigger + Transform
    auto allEntities = registry.getAllEntities();
    // Build list of triggers and targets
    std::vector<entt::entity> triggers;
    std::vector<entt::entity> targets;
    for (auto e : allEntities) {
        if (registry.all_of<TriggerComponent>(e) && registry.all_of<::engine::TransformComponent>(e)) {
            triggers.push_back(e);
        }
        if (registry.all_of<::engine::TransformComponent>(e) && registry.all_of<TagComponent>(e)) {
            targets.push_back(e);
        }
    }

    for (auto trigEntity : triggers) {
        auto& tTransform = registry.get<::engine::TransformComponent>(trigEntity);
        auto& trigger = registry.get<TriggerComponent>(trigEntity);

        std::unordered_set<uint32_t> newOverlaps;
        // Also need to handle cleanup of destroyed entities in currentOverlaps
        // First, remove any prev IDs that are no longer valid
        std::unordered_set<uint32_t> validPrev;
        for (auto id : trigger.currentOverlaps) {
            entt::entity e = static_cast<entt::entity>(id);
            if (registry.valid(e)) validPrev.insert(id);
        }
        trigger.currentOverlaps = std::move(validPrev);

        for (auto tgtEntity : targets) {
            if (trigEntity == tgtEntity) continue;
            const auto& tag = registry.get<TagComponent>(tgtEntity).tag;
            if (!trigger.targetTag.empty() && tag != trigger.targetTag) continue;
            const auto& tgtTransform = registry.get<::engine::TransformComponent>(tgtEntity);
            float dist = glm::distance(tTransform.position, tgtTransform.position);
            if (dist <= trigger.radius) {
                uint32_t tgtId = static_cast<uint32_t>(tgtEntity);
                newOverlaps.insert(tgtId);
                if (trigger.currentOverlaps.find(tgtId) == trigger.currentOverlaps.end()) {
                    // OnEnter
                    EventInboxComponent* inbox = nullptr;
                    if (registry.all_of<EventInboxComponent>(tgtEntity)) {
                        inbox = &registry.get<EventInboxComponent>(tgtEntity);
                    } else {
                        auto& nb = registry.emplace<EventInboxComponent>(tgtEntity);
                        inbox = &nb;
                    }
                    inbox->post(Event{
                        .name = trigger.onEnterEvent,
                        .instigatorEntity = static_cast<uint32_t>(trigEntity),
                        .targetEntity = tgtId,
                        .value = 1.0f
                    });
                }
            }
        }

        // Check for OnExit
        for (uint32_t prevId : trigger.currentOverlaps) {
            if (newOverlaps.find(prevId) == newOverlaps.end()) {
                entt::entity prevEntity = static_cast<entt::entity>(prevId);
                if (registry.valid(prevEntity)) {
                    EventInboxComponent* inbox = nullptr;
                    if (registry.all_of<EventInboxComponent>(prevEntity)) {
                        inbox = &registry.get<EventInboxComponent>(prevEntity);
                    } else {
                        auto& nb = registry.emplace<EventInboxComponent>(prevEntity);
                        inbox = &nb;
                    }
                    inbox->post(Event{
                        .name = trigger.onExitEvent,
                        .instigatorEntity = static_cast<uint32_t>(trigEntity),
                        .targetEntity = prevId,
                        .value = 0.0f
                    });
                }
            }
        }

        trigger.currentOverlaps = std::move(newOverlaps);
    }
}

} // namespace Engine
