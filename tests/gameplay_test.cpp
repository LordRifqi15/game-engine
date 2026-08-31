// Test for Task 034 gameplay graph
// Build: g++ -std=c++17 -I../include ../src/core/gameplay_graph.cpp ../src/core/anim_state_machine.cpp ../src/core/anim_graph.cpp ../src/core/animation_system.cpp gameplay_test.cpp -o gameplay_test

#include "core/gameplay_graph.h"

#include <cstdio>

using namespace engine;

struct BoolHolder : GameplayNode {
    bool v = false;
    bool getBool() const override { return v; }
    void execute(float) override {}
    std::string typeName() const override { return "BoolHolder"; }
    std::unique_ptr<GameplayNode> clone() const override {
        auto up = std::make_unique<BoolHolder>();
        up->v = v;
        return up;
    }
};

int main() {
    AnimParams params;
    auto g = std::make_shared<GameplayGraph>();
    g->target = &params;
    auto* trueNode = g->addNode<FloatNode>(); trueNode->value = 1.5f;
    auto* falseNode = g->addNode<FloatNode>(); falseNode->value = 0.0f;
    auto* boolSrc = g->addNode<BoolHolder>();
    auto* branch = g->addNode<BranchNode>();
    branch->condition = boolSrc;
    branch->trueValue = trueNode;
    branch->falseValue = falseNode;
    auto* setSpeed = g->addNode<SetFloatParamNode>();
    setSpeed->input = branch;
    setSpeed->target = &params;
    setSpeed->member = &AnimParams::speed;

    boolSrc->v = true;
    g->execute(0.016f, params);
    if (params.speed < 1.4f || params.speed > 1.6f) { printf("FAIL branch true %.2f\n", params.speed); return 1; }
    boolSrc->v = false;
    g->execute(0.016f, params);
    if (params.speed != 0.0f) { printf("FAIL branch false %.2f\n", params.speed); return 1; }

    AnimParams p2;
    auto g2 = std::make_shared<GameplayGraph>();
    g2->target = &p2;
    auto* b1 = g2->addNode<BoolHolder>(); b1->v = false;
    auto* b2 = g2->addNode<BoolHolder>(); b2->v = false;
    auto* andNode = g2->addNode<AndNode>(); andNode->a = b1; andNode->b = b2;
    auto* setJump = g2->addNode<SetBoolParamNode>(); setJump->input = andNode; setJump->target = &p2; setJump->member = &AnimParams::jumpPressed;
    b1->v = true; b2->v = true;
    g2->execute(0.016f, p2);
    if (!p2.jumpPressed) { printf("FAIL and true\n"); return 1; }
    // Edge: hold true should give false next frame
    g2->execute(0.016f, p2);
    if (p2.jumpPressed) { printf("FAIL edge hold\n"); return 1; }
    b1->v = false;
    g2->execute(0.016f, p2);
    if (p2.jumpPressed) { printf("FAIL after release\n"); return 1; }
    b1->v = true; b2->v = true;
    g2->execute(0.016f, p2);
    if (!p2.jumpPressed) { printf("FAIL edge re-trigger\n"); return 1; }

    // Timer cycles 0->1->2.5
    AnimParams p3;
    auto g3 = std::make_shared<GameplayGraph>();
    g3->target = &p3;
    auto* timer = g3->addNode<TimerNode>();
    auto* setT = g3->addNode<SetFloatParamNode>(); setT->input = timer; setT->target = &p3; setT->member = &AnimParams::speed;
    for (int i=0;i<125;i++) g3->execute(1.0f/60.0f, p3); // ~2.08s -> should be 1.0
    if (p3.speed < 0.9f || p3.speed > 1.1f) { printf("FAIL timer 2s %.2f\n", p3.speed); return 1; }

    // Minimal graph via makeMinimal still builds and runs
    AnimParams p4;
    auto gm = GameplayGraph::makeMinimal(&p4);
    if (!gm || gm->nodes.size() < 10) { printf("FAIL minimal nodes %zu\n", gm ? gm->nodes.size() : 0); return 1; }
    gm->execute(0.016f, p4); // should not crash

    printf("PASS: gameplay graph Branch/And/Set/Timer/makeMinimal\n");
    return 0;
}
