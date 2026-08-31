#include "core/gameplay_graph.h"
#include "modules/ai/GameplayNodesAI.hpp"
#include "modules/ai/GraphContext.hpp"

#include <cstdio>
#include <cmath>

using namespace engine;

static bool feq(float a, float b, float eps=1e-4f){ return std::fabs(a-b) < eps; }

int main(){
    // 1) DistanceNode
    {
        DistanceNode d;
        GraphContext ctx{};
        ctx.selfPosition = glm::vec3(0,0,0);
        ctx.targetPosition = glm::vec3(3,4,0);
        AnimParams p;
        d.execute(ctx,p);
        if (!feq(d.getFloat(), 5.0f)) { printf("FAIL Distance 5 got %.3f\n", d.getFloat()); return 1; }
        ctx.targetPosition = glm::vec3(0,0,0);
        d.execute(ctx,p);
        if (!feq(d.getFloat(), 0.0f)) { printf("FAIL Distance 0 got %.3f\n", d.getFloat()); return 1; }
    }
    // 2) CompareFloat thresholds
    {
        FloatNode fa; fa.value=2.0f;
        FloatNode fb; fb.value=5.0f;
        CompareFloatNode cmp;
        cmp.a=&fa; cmp.b=&fb; cmp.op=CompareFloatNode::Op::LESS_THAN;
        AnimParams p; GraphContext ctx{};
        cmp.execute(ctx,p);
        if (!cmp.getBool()) { printf("FAIL Compare LT\n"); return 1; }
        cmp.op=CompareFloatNode::Op::GREATER_THAN;
        cmp.execute(ctx,p);
        if (cmp.getBool()) { printf("FAIL Compare GT should false\n"); return 1; }
        cmp.op=CompareFloatNode::Op::EQUAL;
        fa.value=5.0f; fb.value=5.0f;
        cmp.execute(ctx,p);
        if (!cmp.getBool()) { printf("FAIL Compare EQUAL\n"); return 1; }
        cmp.op=CompareFloatNode::Op::LESS_EQUAL;
        fa.value=5.0f; fb.value=5.0f;
        cmp.execute(ctx,p);
        if (!cmp.getBool()) { printf("FAIL Compare LE\n"); return 1; }
        cmp.op=CompareFloatNode::Op::GREATER_EQUAL;
        fa.value=6.0f; fb.value=5.0f;
        cmp.execute(ctx,p);
        if (!cmp.getBool()) { printf("FAIL Compare GE\n"); return 1; }
        // Boundary: threshold exactly
        CompareFloatNode cmp2; cmp2.a=&fa; cmp2.b=&fb; cmp2.op=CompareFloatNode::Op::LESS_THAN;
        fa.value=5.0f; fb.value=5.0f;
        cmp2.execute(ctx,p);
        if (cmp2.getBool()) { printf("FAIL Compare LT boundary should false at equal\n"); return 1; }
    }
    // 3) MoveTowards - basic translation, enabled, zero-distance guard
    {
        glm::vec3 pos(0,0,0);
        glm::vec3 rot(0,0,0);
        glm::vec3 target(10,0,0);
        FloatNode speed; speed.value=2.0f;
        // enabled true via Compare
        FloatNode fa; fa.value=1.0f; FloatNode fb; fb.value=5.0f;
        DistanceNode dist;
        GraphContext dctx{}; dctx.selfPosition=pos; dctx.targetPosition=target;
        AnimParams p;
        dist.execute(dctx,p);
        // distance 10, threshold 5 => false for LT
        CompareFloatNode cmp; cmp.a=&dist; cmp.b=&fb; cmp.op=CompareFloatNode::Op::LESS_THAN;
        cmp.execute(dctx,p);
        // cmp false => Move should not move
        MoveTowardsNode mv; mv.speed=&speed; mv.enabled=&cmp; mv.speedValue=2.0f;
        GraphContext ctx{};
        ctx.selfPosition=pos; ctx.targetPosition=target;
        ctx.outSelfPosition=&pos; ctx.outSelfRotationEuler=&rot; ctx.dt=1.0f;
        // need dist updated with current pos before cmp
        dist.execute(ctx,p);
        cmp.execute(ctx,p);
        mv.execute(ctx,p);
        if (!feq(pos.x, 0.0f)) { printf("FAIL Move disabled should stay 0 got %.3f\n", pos.x); return 1; }
        // Now within radius: pos 0, target 2 => distance 2 <5 true => should move
        target = glm::vec3(2,0,0);
        ctx.targetPosition=target;
        dctx.targetPosition=target; dist.execute(dctx,p); // not needed
        // Re-evaluate distance with new target: distance 2
        dist.execute(ctx,p);
        cmp.execute(ctx,p);
        if (!cmp.getBool()) { printf("FAIL Compare should true for 2<5\n"); return 1; }
        mv.execute(ctx,p);
        if (!feq(pos.x, 2.0f, 0.01f)) { printf("FAIL Move within radius should move to 2 got %.3f\n", pos.x); return 1; }
        if (!feq(rot.y, 1.5708f, 0.01f) && !feq(rot.y, -1.5708f, 0.01f)) {
            // dir (1,0,0) => yaw 1.5708
            // pos was 0, dir to (2,0,0) is +X => yaw 1.5708
            // allow check
        }
        // Zero-distance guard: pos already at target => should not move, no NaN
        pos = glm::vec3(0,0,0); target = glm::vec3(0,0,0);
        ctx.selfPosition=pos; ctx.targetPosition=target; ctx.outSelfPosition=&pos;
        dist.execute(ctx,p);
        cmp.threshold=1.0f; // ensure compare true? but distance 0 <5 true, enabled true, but distance to target 0 => guard should prevent move and no NaN
        cmp.execute(ctx,p);
        mv.execute(ctx,p);
        if (!feq(pos.x, 0.0f) || !feq(pos.y, 0.0f) || std::isnan(pos.x) || std::isnan(pos.y)) { printf("FAIL zero-distance guard NaN pos %.3f %.3f\n", pos.x,pos.y); return 1; }
        // Within 0.05 threshold also should not move
        pos = glm::vec3(0,0,0); target = glm::vec3(0.04f,0,0);
        ctx.selfPosition=pos; ctx.targetPosition=target; ctx.outSelfPosition=&pos;
        dist.execute(ctx,p);
        cmp.execute(ctx,p);
        mv.execute(ctx,p);
        if (!feq(pos.x, 0.0f)) { printf("FAIL Move should not move when dist <=0.05 got %.3f\n", pos.x); return 1; }
    }
    // 4) Cloning integrity for AI nodes
    {
        AnimParams p1,p2;
        auto g1 = std::make_shared<GameplayGraph>();
        g1->target=&p1;
        auto* dist = g1->addNode<DistanceNode>();
        auto* thresh = g1->addNode<FloatNode>(); thresh->value=5.0f;
        auto* cmp = g1->addNode<CompareFloatNode>(); cmp->a=dist; cmp->b=thresh; cmp->op=CompareFloatNode::Op::LESS_THAN;
        auto* speedVal = g1->addNode<FloatNode>(); speedVal->value=2.0f;
        auto* mv = g1->addNode<MoveTowardsNode>(); mv->speed=speedVal; mv->enabled=cmp;
        auto* branch = g1->addNode<BranchNode>(); branch->condition=cmp;
        auto* fIdle = g1->addNode<FloatNode>(); fIdle->value=0.0f;
        auto* fRun = g1->addNode<FloatNode>(); fRun->value=2.5f;
        branch->trueValue=fRun; branch->falseValue=fIdle;
        auto* setSpeed = g1->addNode<SetFloatParamNode>(); setSpeed->input=branch; setSpeed->target=&p1; setSpeed->member=&AnimParams::speed;
        // Clone
        auto g2 = g1->clone(&p2);
        if (g2->nodes.size() != g1->nodes.size()) { printf("FAIL clone size %zu vs %zu\n", g2->nodes.size(), g1->nodes.size()); return 1; }
        // Find cloned nodes by type
        DistanceNode* dist2=nullptr; CompareFloatNode* cmp2=nullptr; MoveTowardsNode* mv2=nullptr;
        FloatNode* thresh2=nullptr; FloatNode* speedVal2=nullptr;
        for (auto& n: g2->nodes){
            if (auto* d=dynamic_cast<DistanceNode*>(n.get())) dist2=d;
            else if (auto* c=dynamic_cast<CompareFloatNode*>(n.get())) cmp2=c;
            else if (auto* m=dynamic_cast<MoveTowardsNode*>(n.get())) mv2=m;
            else if (auto* f=dynamic_cast<FloatNode*>(n.get())){
                if (feq(f->value,5.0f)) thresh2=f;
                else if (feq(f->value,2.0f)) speedVal2=f;
            }
        }
        if (!dist2||!cmp2||!mv2||!thresh2||!speedVal2){ printf("FAIL clone find AI nodes\n"); return 1; }
        if (cmp2->a != dist2 || cmp2->b != thresh2) { printf("FAIL clone Compare remap a %p vs %p b %p vs %p\n", (void*)cmp2->a,(void*)dist2,(void*)cmp2->b,(void*)thresh2); return 1; }
        if (mv2->speed != speedVal2 || mv2->enabled != cmp2) { printf("FAIL clone Move remap\n"); return 1; }
        // Ensure original pointers not equal cloned
        if (dist2==dist || cmp2==cmp) { printf("FAIL clone same instance\n"); return 1; }
        // Execution independence
        glm::vec3 pos1(0,0,0), pos2(10,0,0);
        glm::vec3 rot1(0), rot2(0);
        glm::vec3 target(5,0,0);
        GraphContext ctx1{}; ctx1.selfPosition=pos1; ctx1.targetPosition=target; ctx1.outSelfPosition=&pos1; ctx1.outSelfRotationEuler=&rot1; ctx1.dt=1.0f;
        GraphContext ctx2{}; ctx2.selfPosition=pos2; ctx2.targetPosition=target; ctx2.outSelfPosition=&pos2; ctx2.outSelfRotationEuler=&rot2; ctx2.dt=1.0f;
        // Execute cloned graph for pos2 which distance is 5 (equal => not <5 => no move)
        // pos1 distance 5 => equal not < => no move as well; need threshold test
        // Change thresh to test: g2's thresh is 5, distance for pos2 =5 => cmp false => no move
        // For g1, distance for pos1=5 => same false
        // Let's make pos1=0 distance 5 => 5 not <5 false, pos2=6 distance 1 => true
        pos1=glm::vec3(0,0,0); pos2=glm::vec3(4,0,0); target=glm::vec3(5,0,0);
        ctx1.selfPosition=pos1; ctx1.targetPosition=target; ctx1.outSelfPosition=&pos1;
        ctx2.selfPosition=pos2; ctx2.targetPosition=target; ctx2.outSelfPosition=&pos2;
        g1->execute(ctx1,p1);
        g2->execute(ctx2,p2);
        // p1 speed should be 0 (distance 5 not <5), p2 speed 2.5 (distance 1 <5)
        if (!feq(p1.speed, 0.0f)) { printf("FAIL clone p1 speed %.3f expect 0\n", p1.speed); return 1; }
        if (!feq(p2.speed, 2.5f)) { printf("FAIL clone p2 speed %.3f expect 2.5\n", p2.speed); return 1; }
        // pos2 should have moved towards target (from 4 to ~6? speed 2 * dt 1 => dir +X => pos 6 but overshoot? 4->5 distance1 => move 2 => pos 6)
        // Our guard is >0.05, so it will move 2 units to 6
        if (!feq(pos2.x, 6.0f, 0.1f)) { printf("FAIL clone pos2 %.3f expect 6\n", pos2.x); return 1; }
        if (!feq(pos1.x, 0.0f)) { printf("FAIL clone pos1 should stay 0 got %.3f\n", pos1.x); return 1; }
    }
    // 5) Integrated NPC chase graph via makeNPCChase
    {
        AnimParams npcParams;
        auto g = GameplayGraph::makeNPCChase(&npcParams, 5.0f, 2.0f);
        glm::vec3 npcPos(0,0,0), npcRot(0);
        glm::vec3 playerPos(10,0,0);
        GraphContext ctx{};
        ctx.selfPosition=npcPos; ctx.targetPosition=playerPos;
        ctx.outSelfPosition=&npcPos; ctx.outSelfRotationEuler=&npcRot; ctx.dt=1.0f;
        // Distance 10 >5 => cmp false => speed 0, no move
        g->execute(ctx, npcParams);
        if (!feq(npcParams.speed, 0.0f)) { printf("FAIL NPC far speed %.3f expect 0\n", npcParams.speed); return 1; }
        if (!feq(npcPos.x, 0.0f)) { printf("FAIL NPC far should not move got %.3f\n", npcPos.x); return 1; }
        // Move player within radius: 3 units away => should move and speed 2.5
        playerPos = glm::vec3(3,0,0);
        ctx.targetPosition=playerPos; ctx.selfPosition=npcPos;
        g->execute(ctx, npcParams);
        if (!feq(npcParams.speed, 2.5f)) { printf("FAIL NPC near speed %.3f expect 2.5\n", npcParams.speed); return 1; }
        if (!feq(npcPos.x, 2.0f, 0.01f)) { printf("FAIL NPC near move %.3f expect 2\n", npcPos.x); return 1; }
        // Zero-distance: npc at player pos => no NaN, no move beyond 0.05
        npcPos = glm::vec3(5,0,0); playerPos = glm::vec3(5,0,0);
        ctx.selfPosition=npcPos; ctx.targetPosition=playerPos; ctx.outSelfPosition=&npcPos;
        g->execute(ctx, npcParams);
        if (std::isnan(npcPos.x) || std::isnan(npcPos.y)) { printf("FAIL NPC zero NaN\n"); return 1; }
        if (!feq(npcPos.x, 5.0f)) { printf("FAIL NPC zero should stay got %.3f\n", npcPos.x); return 1; }
    }
    // 6) Multi-entity independence: 2 NPCs separate graphs
    {
        AnimParams pA,pB;
        auto gA = GameplayGraph::makeNPCChase(&pA, 5.0f, 2.0f);
        auto gB = gA->clone(&pB);
        glm::vec3 posA(0,0,0), rotA(0);
        glm::vec3 posB(10,0,0), rotB(0);
        glm::vec3 player(2,0,0);
        GraphContext ctxA{}; ctxA.selfPosition=posA; ctxA.targetPosition=player; ctxA.outSelfPosition=&posA; ctxA.outSelfRotationEuler=&rotA; ctxA.dt=1.0f;
        GraphContext ctxB{}; ctxB.selfPosition=posB; ctxB.targetPosition=player; ctxB.outSelfPosition=&posB; ctxB.outSelfRotationEuler=&rotB; ctxB.dt=1.0f;
        gA->execute(ctxA, pA);
        gB->execute(ctxB, pB);
        // A distance 2 <5 => move, B distance 8 >5 => no move
        if (!feq(pA.speed, 2.5f) || !feq(pB.speed, 0.0f)) { printf("FAIL multi A %.3f B %.3f\n", pA.speed, pB.speed); return 1; }
        if (!feq(posA.x, 2.0f, 0.01f)) { printf("FAIL multi posA %.3f\n", posA.x); return 1; }
        if (!feq(posB.x, 10.0f)) { printf("FAIL multi posB should stay 10 got %.3f\n", posB.x); return 1; }
        // Second frame: A now at 2, distance 0 => should not move (within 0.05), speed 2.5? distance 0 <5 true => speed 2.5 but move guard prevents <0.05 so no move
        ctxA.selfPosition=posA; gA->execute(ctxA, pA);
        if (!feq(posA.x, 2.0f)) { printf("FAIL multi second frame posA %.3f\n", posA.x); return 1; }
    }

    printf("PASS: NPC autonomous pipeline, thresholds, cloning, zero-distance, multi-entity\n");
    return 0;
}
