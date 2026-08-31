#include "core/gameplay_graph.h"

#include "core/anim_editor.h"
#include "modules/ai/GameplayNodesAI.hpp"
#include "platform/input.h"

#include <GLFW/glfw3.h>
#include <unordered_map>

namespace engine {

void FloatNode::execute(float) {
    if (input) value = input->getFloat();
}
std::unique_ptr<GameplayNode> FloatNode::clone() const {
    auto up = std::make_unique<FloatNode>();
    up->value = value;
    up->input = input;
    return up;
}

void KeyInputNode::execute(float) {
    pressed = Input::isKeyPressed(key);
}
std::unique_ptr<GameplayNode> KeyInputNode::clone() const {
    auto up = std::make_unique<KeyInputNode>();
    up->key = key;
    up->pressed = pressed;
    return up;
}

void AndNode::execute(float) {
    bool av = a ? a->getBool() : false;
    bool bv = b ? b->getBool() : false;
    value = av && bv;
}
std::unique_ptr<GameplayNode> AndNode::clone() const {
    auto up = std::make_unique<AndNode>();
    up->value = value;
    up->a = a;
    up->b = b;
    return up;
}

void BranchNode::execute(float) {
    bool c = condition ? condition->getBool() : false;
    float tv = trueValue ? trueValue->getFloat() : 0.0f;
    float fv = falseValue ? falseValue->getFloat() : 0.0f;
    value = c ? tv : fv;
}
std::unique_ptr<GameplayNode> BranchNode::clone() const {
    auto up = std::make_unique<BranchNode>();
    up->value = value;
    up->condition = condition;
    up->trueValue = trueValue;
    up->falseValue = falseValue;
    return up;
}

void SetFloatParamNode::execute(float) {
    if (!target || !input || !member) return;
    target->*member = input->getFloat();
}
std::unique_ptr<GameplayNode> SetFloatParamNode::clone() const {
    auto up = std::make_unique<SetFloatParamNode>();
    up->input = input;
    up->target = target;
    up->member = member;
    return up;
}

void SetBoolParamNode::execute(float) {
    if (!target || !input || !member) return;
    bool cur = input->getBool();
    bool edge = cur && !prev;
    target->*member = edge;
    prev = cur;
}
std::unique_ptr<GameplayNode> SetBoolParamNode::clone() const {
    auto up = std::make_unique<SetBoolParamNode>();
    up->input = input;
    up->target = target;
    up->member = member;
    up->prev = prev;
    return up;
}

void TimerNode::execute(float dt) {
    t += dt;
    if (t < 2.0f) value = 0.0f;
    else if (t < 4.0f) value = 1.0f;
    else if (t < 6.0f) value = 2.5f;
    else { t = 0.0f; value = 0.0f; }
}
std::unique_ptr<GameplayNode> TimerNode::clone() const {
    auto up = std::make_unique<TimerNode>();
    up->value = value;
    return up;
}

void GameplayGraph::setTarget(AnimParams* p) {
    target = p;
    for (auto& n : nodes) {
        if (auto* s = dynamic_cast<SetFloatParamNode*>(n.get())) s->target = p;
        else if (auto* s = dynamic_cast<SetBoolParamNode*>(n.get())) s->target = p;
    }
}
void GameplayGraph::execute(float dt) {
    for (auto& n : nodes) n->execute(dt);
}

void GameplayGraph::execute(float dt, AnimParams& outParams) {
    setTarget(&outParams);
    GraphContext dummy{};
    dummy.dt = dt;
    // Fallback: execute with dummy context so AI nodes get at least dt
    for (auto& n : nodes) n->execute(dummy, outParams);
}

void GameplayGraph::execute(float dt, AnimParams& outParams, const GraphContext& ctx) {
    setTarget(&outParams);
    GraphContext merged = ctx;
    merged.dt = dt;
    for (auto& n : nodes) n->execute(merged, outParams);
}

void GameplayGraph::execute(const GraphContext& ctx, AnimParams& outParams) {
    setTarget(&outParams);
    for (auto& n : nodes) n->execute(ctx, outParams);
}


std::shared_ptr<GameplayGraph> GameplayGraph::clone(AnimParams* newTarget) const {
    auto g = std::make_shared<GameplayGraph>();
    g->target = newTarget;
    std::unordered_map<const GameplayNode*, GameplayNode*> remap;
    for (auto& n : nodes) {
        auto c = n->clone();
        remap[n.get()] = c.get();
        g->nodes.push_back(std::move(c));
    }
    for (size_t i = 0; i < nodes.size(); ++i) {
        auto* src = nodes[i].get();
        auto* dst = g->nodes[i].get();
        if (auto* s = dynamic_cast<FloatNode*>(src)) {
            auto* d = static_cast<FloatNode*>(dst);
            if (s->input) d->input = remap[s->input];
        } else if (auto* s = dynamic_cast<AndNode*>(src)) {
            auto* d = static_cast<AndNode*>(dst);
            if (s->a) d->a = remap[s->a];
            if (s->b) d->b = remap[s->b];
        } else if (auto* s = dynamic_cast<BranchNode*>(src)) {
            auto* d = static_cast<BranchNode*>(dst);
            if (s->condition) d->condition = remap[s->condition];
            if (s->trueValue) d->trueValue = remap[s->trueValue];
            if (s->falseValue) d->falseValue = remap[s->falseValue];
        } else if (auto* s = dynamic_cast<SetFloatParamNode*>(src)) {
            auto* d = static_cast<SetFloatParamNode*>(dst);
            if (s->input) d->input = remap[s->input];
            d->target = newTarget;
        } else if (auto* s = dynamic_cast<SetBoolParamNode*>(src)) {
            auto* d = static_cast<SetBoolParamNode*>(dst);
            if (s->input) d->input = remap[s->input];
            d->target = newTarget;
        } else if (auto* s = dynamic_cast<DistanceNode*>(src)) {
            (void)s; (void)dst; // stateless, no pointers
        } else if (auto* s = dynamic_cast<CompareFloatNode*>(src)) {
            auto* d = static_cast<CompareFloatNode*>(dst);
            if (s->a) d->a = remap[s->a];
            if (s->b) d->b = remap[s->b];
        } else if (auto* s = dynamic_cast<MoveTowardsNode*>(src)) {
            auto* d = static_cast<MoveTowardsNode*>(dst);
            if (s->speed) d->speed = remap[s->speed];
            if (s->enabled) d->enabled = remap[s->enabled];
        } else if (auto* s = dynamic_cast<ApplyImpulseNode*>(src)) {
            auto* d = static_cast<ApplyImpulseNode*>(dst);
            if (s->impulseNode) d->impulseNode = remap[s->impulseNode];
            if (s->trigger) d->trigger = remap[s->trigger];
        }
    }
    return g;
}

std::shared_ptr<GameplayGraph> GameplayGraph::makeMinimal(AnimParams* target) {
    auto g = std::make_shared<GameplayGraph>();
    g->target = target;

    auto* w = g->addNode<KeyInputNode>();
    w->key = GLFW_KEY_W;
    auto* shift = g->addNode<KeyInputNode>();
    shift->key = GLFW_KEY_LEFT_SHIFT;
    auto* space = g->addNode<KeyInputNode>();
    space->key = GLFW_KEY_SPACE;

    auto* andNode = g->addNode<AndNode>();
    andNode->a = w;
    andNode->b = shift;

    auto* float0 = g->addNode<FloatNode>();
    float0->value = 0.0f;
    auto* float15 = g->addNode<FloatNode>();
    float15->value = 1.5f;
    auto* float25 = g->addNode<FloatNode>();
    float25->value = 2.5f;

    auto* branchWalk = g->addNode<BranchNode>();
    branchWalk->condition = w;
    branchWalk->trueValue = float15;
    branchWalk->falseValue = float0;

    auto* branchRun = g->addNode<BranchNode>();
    branchRun->condition = andNode;
    branchRun->trueValue = float25;
    branchRun->falseValue = branchWalk;

    auto* timer = g->addNode<TimerNode>();

    auto* finalBranch = g->addNode<BranchNode>();
    finalBranch->condition = w;
    finalBranch->trueValue = branchRun;
    finalBranch->falseValue = timer;

    auto* setSpeed = g->addNode<SetFloatParamNode>();
    setSpeed->input = finalBranch;
    setSpeed->target = target;
    setSpeed->member = &AnimParams::speed;

    auto* setJump = g->addNode<SetBoolParamNode>();
    setJump->input = space;
    setJump->target = target;
    setJump->member = &AnimParams::jumpPressed;

    // Task 037: real jump via impulse (if physics available, GraphContext will supply outPhysics)
    auto* applyImpulse = g->addNode<ApplyImpulseNode>();
    applyImpulse->trigger = space;
    applyImpulse->impulse = glm::vec3(0.0f, 5.0f, 0.0f);

    return g;
}

std::shared_ptr<GameplayGraph> GameplayGraph::makeNPCChase(AnimParams* target, float detectionRadius, float moveSpeed) {
    auto g = std::make_shared<GameplayGraph>();
    g->target = target;
    // Sensor: distance to player (via GraphContext)
    auto* dist = g->addNode<DistanceNode>();
    // Threshold for detection
    auto* thresh = g->addNode<FloatNode>();
    thresh->value = detectionRadius;
    // Logic: is distance < threshold ?
    auto* cmp = g->addNode<CompareFloatNode>();
    cmp->a = dist;
    cmp->b = thresh;
    cmp->op = CompareFloatNode::Op::LESS_THAN;
    // Animation coupling: speed = cmp ? 2.5 : 0
    auto* floatIdle = g->addNode<FloatNode>();
    floatIdle->value = 0.0f;
    auto* floatRun = g->addNode<FloatNode>();
    floatRun->value = 2.5f;
    auto* speedBranch = g->addNode<BranchNode>();
    speedBranch->condition = cmp;
    speedBranch->trueValue = floatRun;
    speedBranch->falseValue = floatIdle;
    auto* setSpeed = g->addNode<SetFloatParamNode>();
    setSpeed->input = speedBranch;
    setSpeed->target = target;
    setSpeed->member = &AnimParams::speed;
    // Spatial action: move towards target if cmp true
    auto* speedVal = g->addNode<FloatNode>();
    speedVal->value = moveSpeed;
    auto* move = g->addNode<MoveTowardsNode>();
    move->speed = speedVal;
    move->enabled = cmp;
    // Keep move node in graph so it executes; no output needed
    return g;
}

std::shared_ptr<GameplayGraph> buildGameplayGraph(const EditorGraph& ed, AnimParams* target) {
    auto g = std::make_shared<GameplayGraph>();
    g->target = target;
    std::unordered_map<int, GameplayNode*> map;
    for (auto& n : ed.nodes) {
        GameplayNode* node = nullptr;
        if (n.type == "Input") {
            auto* inp = g->addNode<KeyInputNode>();
            inp->key = n.key;
            node = inp;
        } else if (n.type == "Float" || n.type == "Param") {
            auto* f = g->addNode<FloatNode>();
            f->value = n.value;
            node = f;
        } else if (n.type == "And") {
            node = g->addNode<AndNode>();
        } else if (n.type == "Branch") {
            node = g->addNode<BranchNode>();
        } else if (n.type == "SetParam") {
            if (n.targetParam == "jumpPressed") {
                auto* s = g->addNode<SetBoolParamNode>();
                s->target = target;
                s->member = &AnimParams::jumpPressed;
                node = s;
            } else {
                auto* s = g->addNode<SetFloatParamNode>();
                s->target = target;
                s->member = &AnimParams::speed;
                node = s;
            }
        } else if (n.type == "Timer") {
            node = g->addNode<TimerNode>();
        } else if (n.type == "Distance") {
            node = g->addNode<DistanceNode>();
        } else if (n.type == "CompareFloat") {
            auto* c = g->addNode<CompareFloatNode>();
            c->op = CompareFloatNode::opFromString(n.op);
            c->threshold = n.value != 0.0f ? n.value : 5.0f;
            node = c;
        } else if (n.type == "MoveTowards") {
            auto* m = g->addNode<MoveTowardsNode>();
            m->speedValue = n.value != 0.0f ? n.value : 2.0f;
            node = m;
        } else if (n.type == "ApplyImpulse") {
            auto* imp = g->addNode<ApplyImpulseNode>();
            imp->impulse.y = n.value != 0.0f ? n.value : 5.0f;
            node = imp;
        }
    }
    for (auto& l : ed.links) {
        auto itTo = map.find(l.toNode);
        auto itFrom = map.find(l.fromNode);
        if (itTo == map.end() || itFrom == map.end()) continue;
        GameplayNode* to = itTo->second;
        GameplayNode* from = itFrom->second;
        if (auto* b = dynamic_cast<BranchNode*>(to)) {
            if (l.toSlot == 0) b->condition = from;
            else if (l.toSlot == 1) b->trueValue = from;
            else if (l.toSlot == 2) b->falseValue = from;
        } else if (auto* a = dynamic_cast<AndNode*>(to)) {
            if (l.toSlot == 0) a->a = from;
            else if (l.toSlot == 1) a->b = from;
        } else if (auto* s = dynamic_cast<SetFloatParamNode*>(to)) {
            if (l.toSlot == 0) s->input = from;
        } else if (auto* s = dynamic_cast<SetBoolParamNode*>(to)) {
            if (l.toSlot == 0) s->input = from;
        } else if (auto* f = dynamic_cast<FloatNode*>(to)) {
            if (l.toSlot == 0) f->input = from;
        } else if (auto* c = dynamic_cast<CompareFloatNode*>(to)) {
            if (l.toSlot == 0) c->a = from;
            else if (l.toSlot == 1) c->b = from;
        } else if (auto* m = dynamic_cast<MoveTowardsNode*>(to)) {
            if (l.toSlot == 0) m->speed = from;
            else if (l.toSlot == 1) m->enabled = from;
        } else if (auto* imp = dynamic_cast<ApplyImpulseNode*>(to)) {
            if (l.toSlot == 0) imp->impulseNode = from;
            else if (l.toSlot == 1) imp->trigger = from;
        }
    }
    g->setTarget(target);
    return g;
}

EditorGraph makeGameplayEditorGraph() {
    EditorGraph g;
    auto addInput = [&](const std::string& name, int key, float x, float y) {
        EditorNode& n = g.nodes.emplace_back();
        n.id = g.nextId(); n.type = "Input"; n.name = name; n.key = key; n.x = x; n.y = y;
        return n.id;
    };
    auto addFloat = [&](float v, float x, float y) {
        EditorNode& n = g.nodes.emplace_back();
        n.id = g.nextId(); n.type = "Float"; n.value = v; n.x = x; n.y = y;
        return n.id;
    };
    auto addBranch = [&](float x, float y) {
        EditorNode& n = g.nodes.emplace_back();
        n.id = g.nextId(); n.type = "Branch"; n.x = x; n.y = y;
        return n.id;
    };
    auto addAnd = [&](float x, float y) {
        EditorNode& n = g.nodes.emplace_back();
        n.id = g.nextId(); n.type = "And"; n.x = x; n.y = y;
        return n.id;
    };
    auto addSet = [&](const std::string& target, float x, float y) {
        EditorNode& n = g.nodes.emplace_back();
        n.id = g.nextId(); n.type = "SetParam"; n.targetParam = target; n.x = x; n.y = y;
        return n.id;
    };
    auto addTimer = [&](float x, float y) {
        EditorNode& n = g.nodes.emplace_back();
        n.id = g.nextId(); n.type = "Timer"; n.x = x; n.y = y;
        return n.id;
    };

    int wId = addInput("W", GLFW_KEY_W, 40, 40);
    int shiftId = addInput("Shift", GLFW_KEY_LEFT_SHIFT, 40, 140);
    int spaceId = addInput("Space", GLFW_KEY_SPACE, 40, 240);
    int andId = addAnd(200, 80);
    int f0 = addFloat(0.0f, 40, 340);
    int f15 = addFloat(1.5f, 40, 400);
    int f25 = addFloat(2.5f, 40, 460);
    int brWalk = addBranch(200, 200);
    int brRun = addBranch(200, 300);
    int timerId = addTimer(40, 520);
    int brFinal = addBranch(400, 200);
    int setSpeedId = addSet("speed", 600, 200);
    int setJumpId = addSet("jumpPressed", 600, 300);

    g.links.push_back({wId, andId, 0});
    g.links.push_back({shiftId, andId, 1});
    g.links.push_back({wId, brWalk, 0});
    g.links.push_back({f15, brWalk, 1});
    g.links.push_back({f0, brWalk, 2});
    g.links.push_back({andId, brRun, 0});
    g.links.push_back({f25, brRun, 1});
    g.links.push_back({brWalk, brRun, 2});
    g.links.push_back({wId, brFinal, 0});
    g.links.push_back({brRun, brFinal, 1});
    g.links.push_back({timerId, brFinal, 2});
    g.links.push_back({brFinal, setSpeedId, 0});
    g.links.push_back({spaceId, setJumpId, 0});
    g.outputNode = setSpeedId;
    return g;
}

} // namespace engine
