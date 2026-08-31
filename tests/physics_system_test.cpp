#include "core/registry.h"
#include "core/transform_component.h"
#include "ecs/components/PhysicsComponent.hpp"
#include "ecs/components/ColliderComponent.hpp"
#include "modules/physics/PhysicsSystem.hpp"
#include "modules/ai/GraphContext.hpp"
#include "modules/ai/GameplayNodesAI.hpp"
#include "core/gameplay_graph.h"

#include <cstdio>
#include <cmath>

using namespace engine;

static bool feq(float a, float b, float eps=1e-3f){ return std::fabs(a-b) < eps; }

int main(){
    // 1) Freefall & gravity integration
    {
        Registry reg;
        Entity e = reg.createEntity();
        TransformComponent tr; tr.position = glm::vec3(0,10,0);
        reg.addComponent<TransformComponent>(e, tr);
        PhysicsComponent pc;
        pc.velocity = glm::vec3(0);
        pc.mass = 1.0f;
        pc.linearDamping = 0.0f;
        pc.useGravity = true;
        pc.isGrounded = false;
        reg.addComponent<PhysicsComponent>(e, pc);
        ColliderComponent col; col.type = ColliderType::Sphere; col.radius = 0.5f;
        reg.addComponent<ColliderComponent>(e, col);
        PhysicsSystem phys;
        float dt = 1.0f/60.0f;
        // Simulate 60 frames (1 sec)
        for(int i=0;i<60;++i) phys.update(reg, dt);
        auto& tr2 = reg.getComponent<TransformComponent>(e);
        auto& pc2 = reg.getComponent<PhysicsComponent>(e);
        // Velocity should be approx -9.81 after 1 sec
        if (!feq(pc2.velocity.y, -9.81f, 0.5f)) { printf("FAIL gravity velocity %.3f expect -9.81\n", pc2.velocity.y); return 1; }
        // Position should have fallen (y <10)
        if (tr2.position.y >= 10.0f) { printf("FAIL gravity not fallen y %.3f\n", tr2.position.y); return 1; }
        // Should not yet be grounded (ground at 0.5, fallen from 10 for 1 sec ~ 5m, still above)
        if (tr2.position.y <= 0.6f) { printf("FAIL gravity fell too far y %.3f\n", tr2.position.y); return 1; }
    }
    // 2) Ground clamping
    {
        Registry reg;
        Entity e = reg.createEntity();
        TransformComponent tr; tr.position = glm::vec3(0,0.2f,0);
        reg.addComponent<TransformComponent>(e, tr);
        PhysicsComponent pc;
        pc.velocity = glm::vec3(0,-1,0);
        pc.useGravity = true;
        pc.isGrounded = false;
        reg.addComponent<PhysicsComponent>(e, pc);
        ColliderComponent col; col.type = ColliderType::Sphere; col.radius = 0.5f;
        reg.addComponent<ColliderComponent>(e, col);
        PhysicsSystem phys;
        phys.update(reg, 0.016f);
        auto& tr2 = reg.getComponent<TransformComponent>(e);
        auto& pc2 = reg.getComponent<PhysicsComponent>(e);
        if (!feq(tr2.position.y, 0.5f)) { printf("FAIL ground clamp y %.3f expect 0.5\n", tr2.position.y); return 1; }
        if (!feq(pc2.velocity.y, 0.0f)) { printf("FAIL ground velocity y %.3f expect 0\n", pc2.velocity.y); return 1; }
        if (!pc2.isGrounded) { printf("FAIL ground isGrounded false\n"); return 1; }
        // AABB groundOffset test
        Registry reg2;
        Entity e2 = reg2.createEntity();
        TransformComponent trA; trA.position = glm::vec3(0,0.1f,0);
        reg2.addComponent<TransformComponent>(e2, trA);
        PhysicsComponent pcA; pcA.velocity = glm::vec3(0,-2,0); pcA.useGravity=true;
        reg2.addComponent<PhysicsComponent>(e2, pcA);
        ColliderComponent colA; colA.type = ColliderType::AABB; colA.halfExtents = glm::vec3(0.5f,1.0f,0.5f);
        reg2.addComponent<ColliderComponent>(e2, colA);
        phys.update(reg2, 0.016f);
        auto& trA2 = reg2.getComponent<TransformComponent>(e2);
        if (!feq(trA2.position.y, 1.0f)) { printf("FAIL AABB ground y %.3f expect 1.0\n", trA2.position.y); return 1; }
    }
    // 3) Linear damping: horizontal velocity should decay to zero without snapping
    {
        Registry reg;
        Entity e = reg.createEntity();
        TransformComponent tr; tr.position = glm::vec3(0,0.5f,0);
        reg.addComponent<TransformComponent>(e, tr);
        PhysicsComponent pc;
        pc.velocity = glm::vec3(5,0,0);
        pc.linearDamping = 10.0f;
        pc.useGravity = false;
        pc.isGrounded = true;
        reg.addComponent<PhysicsComponent>(e, pc);
        ColliderComponent col; col.radius=0.5f;
        reg.addComponent<ColliderComponent>(e, col);
        PhysicsSystem phys;
        float dt = 0.016f;
        float prevVel = pc.velocity.x;
        for(int i=0;i<120;++i){
            phys.update(reg, dt);
            auto& pc2 = reg.getComponent<PhysicsComponent>(e);
            // Velocity should monotonically decrease, not snap to zero instantly
            if (pc2.velocity.x > prevVel + 1e-5f) { printf("FAIL damping increased %.3f -> %.3f\n", prevVel, pc2.velocity.x); return 1; }
            prevVel = pc2.velocity.x;
            if (std::fabs(pc2.velocity.x) < 0.001f) break;
        }
        auto& pcFinal = reg.getComponent<PhysicsComponent>(e);
        if (std::fabs(pcFinal.velocity.x) > 0.01f) { printf("FAIL damping not zero %.3f\n", pcFinal.velocity.x); return 1; }
        // Ensure no snapping: should take multiple frames, not 1
        // Simulate 1 frame with damping 10, dt 0.016 => factor 0.84, so after 1 frame vel 4.2 not 0
        Registry reg2;
        Entity e2 = reg2.createEntity();
        TransformComponent tr2; tr2.position=glm::vec3(0,0.5f,0); reg2.addComponent<TransformComponent>(e2,tr2);
        PhysicsComponent pc2; pc2.velocity=glm::vec3(5,0,0); pc2.linearDamping=10; pc2.useGravity=false; pc2.isGrounded=true;
        reg2.addComponent<PhysicsComponent>(e2, pc2);
        reg2.addComponent<ColliderComponent>(e2, col);
        phys.update(reg2, dt);
        auto& pcAfter = reg2.getComponent<PhysicsComponent>(e2);
        if (!feq(pcAfter.velocity.x, 5.0f * (1.0f - 10*dt), 0.01f)) { printf("FAIL damping factor %.3f expect %.3f\n", pcAfter.velocity.x, 5*(1-10*dt)); return 1; }
        if (feq(pcAfter.velocity.x, 0.0f)) { printf("FAIL damping snapped to zero\n"); return 1; }
    }
    // 4) Velocity movement without snapping (MoveTowards via physics)
    {
        Registry reg;
        Entity npc = reg.createEntity();
        TransformComponent tr; tr.position = glm::vec3(0,0.5f,0);
        reg.addComponent<TransformComponent>(npc, tr);
        PhysicsComponent pc; pc.velocity=glm::vec3(0); pc.linearDamping=0; pc.useGravity=false; pc.isGrounded=true;
        reg.addComponent<PhysicsComponent>(npc, pc);
        ColliderComponent col; col.radius=0.5f; reg.addComponent<ColliderComponent>(npc, col);
        // Create chase graph: Distance -> Compare (<5) -> MoveTowards speed 2
        AnimParams params;
        auto g = GameplayGraph::makeNPCChase(&params, 5.0f, 2.0f);
        glm::vec3 playerPos(10,0.5f,0);
        // First frame: npc at 0, player at 10, distance 10 >5 => no move, velocity 0
        {
            auto& trNpc = reg.getComponent<TransformComponent>(npc);
            auto& phys = reg.getComponent<PhysicsComponent>(npc);
            GraphContext ctx{};
            ctx.selfPosition = trNpc.position;
            ctx.targetPosition = playerPos;
            ctx.outPhysics = &phys;
            ctx.dt = 0.016f;
            g->execute(ctx, params);
            // MoveTowards should not set velocity because enabled false (distance >5)
            if (!feq(phys.velocity.x, 0.0f) || !feq(phys.velocity.z, 0.0f)) { printf("FAIL NPC far velocity %.3f %.3f expect 0\n", phys.velocity.x, phys.velocity.z); return 1; }
        }
        // Move player within radius: 2 units away
        playerPos = glm::vec3(2,0.5f,0);
        {
            auto& trNpc = reg.getComponent<TransformComponent>(npc);
            auto& phys = reg.getComponent<PhysicsComponent>(npc);
            GraphContext ctx{};
            ctx.selfPosition = trNpc.position;
            ctx.targetPosition = playerPos;
            ctx.outPhysics = &phys;
            ctx.dt = 0.016f;
            g->execute(ctx, params);
            if (!feq(phys.velocity.x, 2.0f, 0.01f)) { printf("FAIL NPC near velocity x %.3f expect 2\n", phys.velocity.x); return 1; }
        }
        // Now physics integrate should move npc without snapping
        PhysicsSystem ps;
        ps.update(reg, 0.016f);
        float oldX = reg.getComponent<TransformComponent>(npc).position.x;
        if (!feq(oldX, 0.0f + 2.0f*0.016f, 0.001f)) { printf("FAIL NPC position after physics %.3f expect %.3f\n", oldX, 2*0.016f); return 1; }
        // Second frame: npc should continue moving, no jitter (velocity consistent)
        {
            auto& phys = reg.getComponent<PhysicsComponent>(npc);
            // Keep same player pos, npc now at 0.032, distance still <5, so velocity stays 2
            GraphContext ctx{};
            ctx.selfPosition = glm::vec3(oldX, 0.5f, 0);
            ctx.targetPosition = playerPos;
            ctx.outPhysics = &phys;
            ctx.dt = 0.016f;
            g->execute(ctx, params);
            if (!feq(phys.velocity.x, 2.0f, 0.01f)) { printf("FAIL NPC second frame velocity %.3f\n", phys.velocity.x); return 1; }
        }
        ps.update(reg, 0.016f);
        auto& trAfter2 = reg.getComponent<TransformComponent>(npc);
        if (trAfter2.position.x <= oldX) { printf("FAIL NPC not moving forward %.3f <= %.3f\n", trAfter2.position.x, oldX); return 1; }
        if (std::isnan(trAfter2.position.x)) { printf("FAIL NPC NaN\n"); return 1; }
    }
    // 5) ApplyImpulse rising edge and grounded check
    {
        Registry reg;
        Entity e = reg.createEntity();
        TransformComponent tr; tr.position = glm::vec3(0,0.5f,0); reg.addComponent<TransformComponent>(e, tr);
        PhysicsComponent pc; pc.velocity=glm::vec3(0); pc.mass=1; pc.isGrounded=true; pc.useGravity=true;
        reg.addComponent<PhysicsComponent>(e, pc);
        ColliderComponent col; col.radius=0.5f; reg.addComponent<ColliderComponent>(e, col);
        // Build graph: KeyInput (space) -> ApplyImpulse
        // Simulate trigger bool via Compare or direct KeyInput? Use BoolHolder
        auto g = std::make_shared<GameplayGraph>();
        AnimParams params;
        g->target=&params;
        auto* trigger = g->addNode<FloatNode>(); // will use as bool via getBool? Need BoolHolder
        // Use simple FloatNode as trigger? ApplyImpulse expects trigger->getBool(), so need Bool node.
        // Create a manual BoolHolder via FloatNode? Instead directly test ApplyImpulse node in isolation
        ApplyImpulseNode imp;
        imp.impulse = glm::vec3(0,5,0);
        // Create a trigger node that returns true
        struct BoolTrue : GameplayNode {
            bool getBool() const override { return true; }
            void execute(float) override {}
            void execute(const GraphContext&, AnimParams&) override {}
            std::string typeName() const override { return "BoolTrue"; }
            std::unique_ptr<GameplayNode> clone() const override { return std::make_unique<BoolTrue>(); }
        };
        BoolTrue trig;
        imp.trigger = &trig;
        imp.prevTrigger = false;
        auto& phys = reg.getComponent<PhysicsComponent>(e);
        GraphContext ctx{}; ctx.outPhysics=&phys; ctx.dt=0.016f;
        AnimParams p;
        imp.execute(ctx, p);
        if (!feq(phys.velocity.y, 5.0f)) { printf("FAIL impulse velocity %.3f expect 5\n", phys.velocity.y); return 1; }
        // Second execute with still true should not re-apply (rising edge)
        imp.execute(ctx, p);
        if (!feq(phys.velocity.y, 5.0f)) { printf("FAIL impulse re-triggered %.3f\n", phys.velocity.y); return 1; }
        // Release trigger then re-trigger
        struct BoolFalse : GameplayNode {
            bool getBool() const override { return false; }
            void execute(float) override {}
            void execute(const GraphContext&, AnimParams&) override {}
            std::string typeName() const override { return "BoolFalse"; }
            std::unique_ptr<GameplayNode> clone() const override { return std::make_unique<BoolFalse>(); }
        };
        BoolFalse trigFalse;
        imp.trigger = &trigFalse;
        imp.execute(ctx, p);
        // Now rising again
        imp.trigger = &trig;
        // Make not grounded => should not apply
        phys.isGrounded = false;
        phys.velocity.y = 0;
        imp.execute(ctx, p);
        if (!feq(phys.velocity.y, 0.0f)) { printf("FAIL impulse when not grounded should 0 got %.3f\n", phys.velocity.y); return 1; }
        phys.isGrounded = true;
        // Need to reset prevTrigger to false via false trigger
        imp.trigger = &trigFalse; imp.execute(ctx,p);
        imp.trigger = &trig; imp.execute(ctx,p);
        if (!feq(phys.velocity.y, 5.0f)) { printf("FAIL impulse after grounded %.3f\n", phys.velocity.y); return 1; }
    }

    printf("PASS: physics system gravity, damping, ground, NPC velocity, impulse\n");
    return 0;
}
