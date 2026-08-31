#include "core/registry.h"
#include "core/transform_component.h"
#include "ecs/components/BlackboardComponent.hpp"
#include "ecs/components/PhysicsComponent.hpp"
#include "ecs/components/ColliderComponent.hpp"
#include "modules/physics/PhysicsSystem.hpp"
#include "modules/ai/GraphContext.hpp"
#include "modules/ai/BlackboardNodes.hpp"
#include "modules/ai/GameplayNodesAI.hpp"
#include "core/gameplay_graph.h"

#include <cstdio>
#include <cmath>

using namespace engine;

static bool feq(float a, float b, float eps=1e-3f){ return std::fabs(a-b) < eps; }
static bool veq(const glm::vec3& a, const glm::vec3& b, float eps=1e-3f){
    return feq(a.x,b.x,eps) && feq(a.y,b.y,eps) && feq(a.z,b.z,eps);
}

int main(){
    // 1) Blackboard key accessors and fallbacks
    {
        BlackboardComponent bb;
        bb.setFloat("f", 1.5f);
        bb.setBool("b", true);
        bb.setVec3("v", glm::vec3(1,2,3));
        if (!feq(bb.getFloat("f"), 1.5f)) { printf("FAIL getFloat\n"); return 1; }
        if (!feq(bb.getFloat("missing", 2.0f), 2.0f)) { printf("FAIL getFloat fallback\n"); return 1; }
        if (!bb.getBool("b")) { printf("FAIL getBool\n"); return 1; }
        if (bb.getBool("missing", true)!=true) { printf("FAIL getBool fallback\n"); return 1; }
        if (!veq(bb.getVec3("v"), glm::vec3(1,2,3))) { printf("FAIL getVec3\n"); return 1; }
        if (!veq(bb.getVec3("missing", glm::vec3(5,5,5)), glm::vec3(5,5,5))) { printf("FAIL getVec3 fallback\n"); return 1; }
        if (!bb.hasKey("f") || bb.hasKey("nope")) { printf("FAIL hasKey\n"); return 1; }
    }
    // 2) Set/Get nodes with GraphContext
    {
        Registry reg;
        Entity e = reg.createEntity();
        BlackboardComponent bb;
        reg.addComponent<BlackboardComponent>(e, bb);
        auto& b = reg.getComponent<BlackboardComponent>(e);
        // Set via node
        SetBlackboardFloatNode setF; setF.key="score"; setF.defaultValue=0;
        FloatNode src; src.value=3.14f; setF.value=&src;
        GraphContext ctx{}; ctx.blackboard=&b; ctx.dt=0.016f;
        AnimParams p;
        setF.execute(ctx,p);
        if (!feq(b.getFloat("score"), 3.14f)) { printf("FAIL SetGet Float\n"); return 1; }
        // Get via node
        GetBlackboardFloatNode getF; getF.key="score"; getF.defaultValue=0;
        getF.execute(ctx,p);
        if (!feq(getF.getFloat(), 3.14f)) { printf("FAIL GetFloat cached %.3f\n", getF.getFloat()); return 1; }
        // Set Vec3
        SetBlackboardVec3Node setV; setV.key="pos"; setV.defaultValue=glm::vec3(0);
        FloatNode dummy; // not used, but test vec3 via direct default
        // Use condition false should not set
        SetBlackboardBoolNode setB; setB.key="flag"; setB.defaultValue=true;
        BoolHolderFalse: ; // use condition node
        // Create bool condition false
        struct BoolFalse : GameplayNode {
            bool getBool() const override { return false; }
            void execute(float) override {}
            void execute(const GraphContext&, AnimParams&) override {}
            std::string typeName() const override { return "BoolFalse"; }
            std::unique_ptr<GameplayNode> clone() const override { return std::make_unique<BoolFalse>(); }
        } bf;
        setB.value=nullptr; setB.condition=&bf;
        setB.execute(ctx,p); // bf false => should not set
        if (b.hasKey("flag")) { printf("FAIL SetBool condition false should not set\n"); return 1; }
        // Now true condition
        struct BoolTrue : GameplayNode {
            bool getBool() const override { return true; }
            void execute(float) override {}
            void execute(const GraphContext&, AnimParams&) override {}
            std::string typeName() const override { return "BoolTrue"; }
            std::unique_ptr<GameplayNode> clone() const override { return std::make_unique<BoolTrue>(); }
        } bt;
        setB.condition=&bt; setB.execute(ctx,p);
        if (!b.getBool("flag")) { printf("FAIL SetBool true\n"); return 1; }
        // Get Bool
        GetBlackboardBoolNode getB; getB.key="flag";
        getB.execute(ctx,p);
        if (!getB.getBool()) { printf("FAIL GetBool\n"); return 1; }
        // Null blackboard guard
        GraphContext nullCtx{}; nullCtx.blackboard=nullptr;
        SetBlackboardFloatNode setNull; setNull.key="x"; setNull.defaultValue=1; setNull.execute(nullCtx,p); // should no-op no crash
        GetBlackboardFloatNode getNull; getNull.key="x"; getNull.execute(nullCtx,p);
        if (!feq(getNull.getFloat(), 0.0f)) { /* default 0 */ }
    }
    // 3) StateTimerNode
    {
        StateTimerNode timer;
        timer.duration=1.0f;
        struct BoolFalse : GameplayNode {
            bool getBool() const override { return false; }
            void execute(float) override {}
            void execute(const GraphContext&, AnimParams&) override {}
            std::string typeName() const override { return "BoolFalse"; }
            std::unique_ptr<GameplayNode> clone() const override { return std::make_unique<BoolFalse>(); }
        } bf;
        struct BoolTrue : GameplayNode {
            bool getBool() const override { return true; }
            void execute(float) override {}
            void execute(const GraphContext&, AnimParams&) override {}
            std::string typeName() const override { return "BoolTrue"; }
            std::unique_ptr<GameplayNode> clone() const override { return std::make_unique<BoolTrue>(); }
        } bt;
        timer.reset=&bf;
        AnimParams p; GraphContext ctx{}; ctx.dt=0.5f;
        timer.execute(ctx,p);
        if (timer.getBool()) { printf("FAIL timer not finished at 0.5\n"); return 1; }
        timer.execute(ctx,p); // 1.0
        if (!timer.getBool()) { printf("FAIL timer should finished at 1.0\n"); return 1; }
        if (!feq(timer.getFloat(), 1.0f)) { printf("FAIL timer elapsed %.3f\n", timer.getFloat()); return 1; }
        // Reset
        timer.reset=&bt;
        timer.execute(ctx,p);
        if (timer.getBool()) { printf("FAIL timer after reset should not finished\n"); return 1; }
        if (!feq(timer.getFloat(), 0.0f)) { printf("FAIL timer after reset elapsed %.3f\n", timer.getFloat()); return 1; }
        // Clone should reset elapsed
        timer.elapsed=2.0f; timer.isFinished=true;
        auto c = timer.clone();
        auto* tc = dynamic_cast<StateTimerNode*>(c.get());
        if (!tc || !feq(tc->elapsed,0.0f) || tc->isFinished) { printf("FAIL timer clone reset\n"); return 1; }
    }
    // 4) Full blackboard chase flow: Chase -> Investigate -> Idle
    {
        Registry reg;
        Entity player = reg.createEntity();
        TransformComponent pt; pt.position=glm::vec3(0,0.5f,0); reg.addComponent<TransformComponent>(player, pt);
        Entity npc = reg.createEntity();
        TransformComponent nt; nt.position=glm::vec3(10,0.5f,0); reg.addComponent<TransformComponent>(npc, nt);
        PhysicsComponent pc; pc.velocity=glm::vec3(0); pc.linearDamping=0; pc.useGravity=false; pc.isGrounded=true;
        reg.addComponent<PhysicsComponent>(npc, pc);
        BlackboardComponent bb; reg.addComponent<BlackboardComponent>(npc, bb);
        ColliderComponent col; col.radius=0.5f; reg.addComponent<ColliderComponent>(npc, col);
        AnimParams params;
        auto g = GameplayGraph::makeBlackboardChase(&params, 6.0f, 3.0f, 2.0f); // detection 6, speed 3, wait 2
        auto& b = reg.getComponent<BlackboardComponent>(npc);
        auto& tr = reg.getComponent<TransformComponent>(npc);
        auto& phys = reg.getComponent<PhysicsComponent>(npc);
        // Phase 1: player far at (20,0.5,0) distance 10 >6 => IsSearching false, speed 0, no move
        glm::vec3 playerPos(20,0.5f,0);
        {
            GraphContext ctx{};
            ctx.selfPosition=tr.position; ctx.targetPosition=playerPos;
            ctx.outPhysics=&phys; ctx.blackboard=&b; ctx.dt=0.016f;
            g->execute(ctx, params);
        }
        if (b.getBool("IsSearching", false)) { printf("FAIL phase1 IsSearching should false when far\n"); return 1; }
        if (!feq(params.speed, 0.0f)) { printf("FAIL phase1 speed %.3f expect 0\n", params.speed); return 1; }
        // Phase 2: player moves within 6 (3,0.5,0) distance 7? npc at 10, player 3 distance 7 >6 still false? Let's use player at 8 distance 2 <6
        playerPos = glm::vec3(8,0.5f,0);
        {
            GraphContext ctx{};
            ctx.selfPosition=tr.position; ctx.targetPosition=playerPos;
            ctx.outPhysics=&phys; ctx.blackboard=&b; ctx.dt=0.016f;
            g->execute(ctx, params);
        }
        if (!b.getBool("IsSearching")) { printf("FAIL phase2 IsSearching should true when canSee\n"); return 1; }
        if (!veq(b.getVec3("LastSeenPos"), playerPos)) { printf("FAIL LastSeenPos not updated\n"); return 1; }
        if (!feq(params.speed, 2.5f)) { printf("FAIL phase2 speed %.3f expect 2.5\n", params.speed); return 1; }
        if (!feq(phys.velocity.x, -3.0f, 0.01f)) { // dir from 10 to 8 is -1 *3 => -3
            printf("FAIL phase2 velocity x %.3f expect -3\n", phys.velocity.x); return 1;
        }
        // Simulate physics move one frame
        PhysicsSystem ps;
        // Need to have transform and physics in registry for ps.update to move
        // Update registry's transform/physics already, but we used local tr reference, need to update registry's copy
        // Use registry's transform directly
        ps.update(reg, 0.016f);
        auto& tr2 = reg.getComponent<TransformComponent>(npc);
        float movedX = tr2.position.x;
        if (movedX >= 10.0f) { printf("FAIL npc not moved %.3f\n", movedX); return 1; }
        // Phase 3: player moves out of range again to (20,0.5,0), npc should continue to LastSeenPos (8) not stop immediately (target memory)
        playerPos = glm::vec3(20,0.5f,0);
        // NPC at ~9.95, LastSeenPos is 8, distance to player is ~10, canSee false, but IsSearching still true, so should continue to 8
        {
            auto& tr3 = reg.getComponent<TransformComponent>(npc);
            auto& phys3 = reg.getComponent<PhysicsComponent>(npc);
            auto& bb3 = reg.getComponent<BlackboardComponent>(npc);
            GraphContext ctx{};
            ctx.selfPosition=tr3.position; ctx.targetPosition=playerPos;
            ctx.outPhysics=&phys3; ctx.blackboard=&bb3; ctx.dt=0.016f;
            AnimParams p2;
            // Need to use same graph but with new params? Use params from previous, but blackboard is per-entity
            // For this test, reuse g with new params reference (target was params, but we need to set target to p2)
            // Instead create a cloned graph for this frame with new params
            auto g2 = g->clone(&p2);
            // Need to find the IsSearching bool after clone? The blackboard is same, so IsSearching still true
            g2->execute(ctx, p2);
            // Should still be searching and moving towards LastSeenPos 8, not player 20
            // Check velocity direction: target for MoveTowards when IsSearching true is LastSeenPos 8, self at ~9.95 dir is -1, so velocity negative
            if (phys3.velocity.x >= 0) { printf("FAIL phase3 should move towards LastSeenPos 8, vel %.3f\n", phys3.velocity.x); return 1; }
            // IsSearching should still true before timer finishes
            if (!bb3.getBool("IsSearching")) { printf("FAIL phase3 IsSearching should still true before timer\n"); return 1; }
        }
        // Simulate until timer finishes: need to run for waitDuration 2 sec without seeing player
        // Timer reset is canSee (distance <6). Since player far, canSee false, timer will accumulate
        // After 2 sec, IsSearching should become false
        {
            // Run 2 sec worth of frames (125 frames)
            AnimParams pTmp;
            auto gTmp = g->clone(&pTmp);
            // Use the npc's blackboard and physics from registry
            for(int i=0;i<130;++i){
                auto& trTmp = reg.getComponent<TransformComponent>(npc);
                auto& physTmp = reg.getComponent<PhysicsComponent>(npc);
                auto& bbTmp = reg.getComponent<BlackboardComponent>(npc);
                GraphContext ctx{};
                ctx.selfPosition=trTmp.position; ctx.targetPosition=playerPos; // player still far
                ctx.outPhysics=&physTmp; ctx.blackboard=&bbTmp; ctx.dt=0.016f;
                gTmp->execute(ctx, pTmp);
                // Physics would move, but we are testing blackboard logic only, not position
                // Also need to handle that after timer, IsSearching false, so speed 0
            }
            auto& bbFinal = reg.getComponent<BlackboardComponent>(npc);
            if (bbFinal.getBool("IsSearching")) { printf("FAIL timed searching should have reset IsSearching after 2 sec\n"); return 1; }
        }
    }
    // 5) Multi-NPC isolation
    {
        Registry reg;
        Entity player = reg.createEntity();
        TransformComponent pt; pt.position=glm::vec3(0,0.5f,0); reg.addComponent<TransformComponent>(player, pt);
        Entity npcA = reg.createEntity();
        TransformComponent ta; ta.position=glm::vec3(10,0.5f,0); reg.addComponent<TransformComponent>(npcA, ta);
        PhysicsComponent pa; pa.velocity=glm::vec3(0); reg.addComponent<PhysicsComponent>(npcA, pa);
        BlackboardComponent bba; reg.addComponent<BlackboardComponent>(npcA, bba);
        Entity npcB = reg.createEntity();
        TransformComponent tb; tb.position=glm::vec3(-10,0.5f,0); reg.addComponent<TransformComponent>(npcB, tb);
        PhysicsComponent pb; pb.velocity=glm::vec3(0); reg.addComponent<PhysicsComponent>(npcB, pb);
        BlackboardComponent bbb; reg.addComponent<BlackboardComponent>(npcB, bbb);
        AnimParams paramsA, paramsB;
        auto gA = GameplayGraph::makeBlackboardChase(&paramsA, 6.0f, 2.0f, 2.0f);
        auto gB = gA->clone(&paramsB);
        // Player at (5,0.5,0) => npcA distance 5 <6 canSee true, npcB distance 15 >6 false
        glm::vec3 playerPos(5,0.5f,0);
        {
            auto& trA = reg.getComponent<TransformComponent>(npcA);
            auto& physA = reg.getComponent<PhysicsComponent>(npcA);
            auto& bbA = reg.getComponent<BlackboardComponent>(npcA);
            GraphContext ctxA{}; ctxA.selfPosition=trA.position; ctxA.targetPosition=playerPos; ctxA.outPhysics=&physA; ctxA.blackboard=&bbA; ctxA.dt=0.016f;
            gA->execute(ctxA, paramsA);
            auto& trB = reg.getComponent<TransformComponent>(npcB);
            auto& physB = reg.getComponent<PhysicsComponent>(npcB);
            auto& bbB = reg.getComponent<BlackboardComponent>(npcB);
            GraphContext ctxB{}; ctxB.selfPosition=trB.position; ctxB.targetPosition=playerPos; ctxB.outPhysics=&physB; ctxB.blackboard=&bbB; ctxB.dt=0.016f;
            gB->execute(ctxB, paramsB);
            if (!bbA.getBool("IsSearching")) { printf("FAIL multi A should be searching\n"); return 1; }
            if (bbB.getBool("IsSearching")) { printf("FAIL multi B should not be searching\n"); return 1; }
            if (!veq(bbA.getVec3("LastSeenPos"), playerPos)) { printf("FAIL multi A LastSeenPos\n"); return 1; }
            if (bbB.hasKey("LastSeenPos") && veq(bbB.getVec3("LastSeenPos"), playerPos)) { printf("FAIL multi B should not have LastSeenPos as A\n"); return 1; }
        }
        // Now move player to (-9,0.5,0) => npcB should see, npcA not
        playerPos = glm::vec3(-9,0.5f,0);
        {
            auto& trA = reg.getComponent<TransformComponent>(npcA);
            auto& physA = reg.getComponent<PhysicsComponent>(npcA);
            auto& bbA = reg.getComponent<BlackboardComponent>(npcA);
            GraphContext ctxA{}; ctxA.selfPosition=trA.position; ctxA.targetPosition=playerPos; ctxA.outPhysics=&physA; ctxA.blackboard=&bbA; ctxA.dt=0.016f;
            gA->execute(ctxA, paramsA);
            auto& trB = reg.getComponent<TransformComponent>(npcB);
            auto& physB = reg.getComponent<PhysicsComponent>(npcB);
            auto& bbB = reg.getComponent<BlackboardComponent>(npcB);
            GraphContext ctxB{}; ctxB.selfPosition=trB.position; ctxB.targetPosition=playerPos; ctxB.outPhysics=&physB; ctxB.blackboard=&bbB; ctxB.dt=0.016f;
            gB->execute(ctxB, paramsB);
            // After this, B should be searching, A should still be searching (since its timer not yet finished, but canSee false)
            // But we haven't waited, so A still searching true, B now true, both true is okay, but they should have distinct LastSeenPos
            if (!veq(bbB.getVec3("LastSeenPos"), playerPos)) { printf("FAIL multi B LastSeenPos after second\n"); return 1; }
            // A's LastSeenPos should still be (5,0.5,0) from before, not overwritten to (-9)
            if (!veq(bbA.getVec3("LastSeenPos"), glm::vec3(5,0.5f,0))) { printf("FAIL multi A LastSeenPos should remain 5, got %.1f\n", bbA.getVec3("LastSeenPos").x); return 1; }
        }
    }

    printf("PASS: blackboard persistence, timers, isolation\n");
    return 0;
}
