#include "core/gameplay_graph.h"

#include "core/anim_editor.h"
#include "platform/input.h"

#include <GLFW/glfw3.h>
#include <unordered_map>
namespace engine {

void FloatNode::execute(float) {
    if (input) value = input->getFloat();
}

void KeyInputNode::execute(float) {
    pressed = Input::isKeyPressed(key);
}

void AndNode::execute(float) {
    bool av = a ? a->getBool() : false;
    bool bv = b ? b->getBool() : false;
    value = av && bv;
}

void BranchNode::execute(float) {
    bool c = condition ? condition->getBool() : false;
    float tv = trueValue ? trueValue->getFloat() : 0.0f;
    float fv = falseValue ? falseValue->getFloat() : 0.0f;
    value = c ? tv : fv;
}

void SetFloatParamNode::execute(float) {
    if (!target || !input || !member) return;
    target->*member = input->getFloat();
}

void SetBoolParamNode::execute(float) {
    if (!target || !input || !member) return;
    bool cur = input->getBool();
    bool edge = cur && !prev;
    target->*member = edge;
    prev = cur;
}

void TimerNode::execute(float dt) {
    t += dt;
    if (t < 2.0f) value = 0.0f;
    else if (t < 4.0f) value = 1.0f;
    else if (t < 6.0f) value = 2.5f;
    else { t = 0.0f; value = 0.0f; }
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
    execute(dt);
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

    // hasInput = W (for minimal, if W pressed use input, else timer)
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
        }
        if (node) map[n.id] = node;
    }
    // Wire links: for gameplay, toSlot 0 = input/condition, 1 = trueValue/B, 2 = falseValue/param etc
    // For Branch: 0=condition, 1=trueValue, 2=falseValue
    // For And: 0=a, 1=b
    // For SetParam: 0=input
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
        }
    }
    // Update Set* targets to current target (in case it changed)
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
    // No output needed for gameplay graph (it writes to params), but set one for completeness
    g.outputNode = setSpeedId;
    return g;
}

} // namespace engine
