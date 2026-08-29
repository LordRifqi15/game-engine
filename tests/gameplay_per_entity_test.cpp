// Test for Task 035 per-entity gameplay graphs
// Build: g++ -std=c++17 -I../include ../src/core/gameplay_graph.cpp ../src/core/anim_state_machine.cpp ../src/core/anim_graph.cpp ../src/core/animation_system.cpp gameplay_per_entity_test.cpp -o gameplay_per_entity_test

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
    AnimParams p1, p2;
    auto templ = GameplayGraph::makeMinimal(nullptr);
    auto g1 = templ->clone(&p1);
    auto g2 = templ->clone(&p2);
    if (!g1 || !g2) { printf("FAIL clone\n"); return 1; }
    if (g1.get() == g2.get()) { printf("FAIL same instance\n"); return 1; }
    if (g1->nodes.size() != g2->nodes.size()) { printf("FAIL nodes size\n"); return 1; }

    for (int i=0;i<125;i++) { g1->execute(1.0f/60.0f, p1); g2->execute(1.0f/60.0f, p2); }
    if (p1.speed < 0.9f || p1.speed > 1.1f) { printf("FAIL g1 timer %.2f\n", p1.speed); return 1; }
    if (p2.speed < 0.9f || p2.speed > 1.1f) { printf("FAIL g2 timer %.2f\n", p2.speed); return 1; }

    float p2_before = p2.speed;
    for (int i=0;i<60;i++) g1->execute(1.0f/60.0f, p1);
    if (p2.speed != p2_before) { printf("FAIL cross-entity leak p2 %.2f vs %.2f\n", p2.speed, p2_before); return 1; }

    // Edge per-entity: use custom graph with BoolHolder -> SetBool
    AnimParams pa, pb;
    auto ga = std::make_shared<GameplayGraph>();
    ga->target = &pa;
    auto* b1 = ga->addNode<BoolHolder>(); b1->v = false;
    auto* b2 = ga->addNode<BoolHolder>(); b2->v = false;
    auto* andNode = ga->addNode<AndNode>(); andNode->a = b1; andNode->b = b2;
    auto* setA = ga->addNode<SetBoolParamNode>(); setA->input = andNode; setA->target = &pa; setA->member = &AnimParams::jumpPressed;
    auto gb = ga->clone(&pb);
    // Find BoolHolders in gb
    BoolHolder* gb_b1 = nullptr;
    BoolHolder* gb_b2 = nullptr;
    for (auto& n : gb->nodes) if (auto* h = dynamic_cast<BoolHolder*>(n.get())) { if (!gb_b1) gb_b1 = h; else if (!gb_b2) gb_b2 = h; }
    if (!gb_b1 || !gb_b2) { printf("FAIL find holders in clone\n"); return 1; }
    b1->v = true; b2->v = true;
    ga->execute(0.016f, pa);
    if (!pa.jumpPressed) { printf("FAIL ga true\n"); return 1; }
    ga->execute(0.016f, pa);
    if (pa.jumpPressed) { printf("FAIL ga edge hold\n"); return 1; }
    gb_b1->v = true; gb_b2->v = true;
    gb->execute(0.016f, pb);
    if (!pb.jumpPressed) { printf("FAIL gb true\n"); return 1; }
    // Ensure ga's edge state didn't leak to gb's next frame
    ga->execute(0.016f, pa);
    if (pa.jumpPressed) { printf("FAIL ga should stay false after edge\n"); return 1; }
    gb->execute(0.016f, pb);
    if (pb.jumpPressed) { printf("FAIL gb edge hold\n"); return 1; }

    printf("PASS: per-entity gameplay graphs independent\n");
    return 0;
}
