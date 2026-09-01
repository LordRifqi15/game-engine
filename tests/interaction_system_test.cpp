// InteractionSystem: Enter/Stay/Exit + OnEvent/Emit/Modify + multi-entity + attack flow
// Build: g++ -std=c++17 -I../include -I../third_party ../src/modules/interaction/InteractionSystem.cpp ../src/modules/interaction/InteractionNodes.cpp ../src/core/gameplay_graph.cpp ../src/core/anim_graph.cpp ../src/core/anim_state_machine.cpp ../src/core/animation_system.cpp ../src/modules/ai/GameplayNodesAI.cpp ../src/modules/ai/BlackboardNodes.cpp ../src/modules/navigation/NavGrid.cpp ../src/modules/navigation/Pathfinding.cpp ../src/modules/navigation/NavigationNodes.cpp interaction_system_test.cpp -o /tmp/interaction_system_test && /tmp/interaction_system_test

#include "core/registry.h"
#include "core/transform_component.h"
#include "core/gameplay_graph.h"
#include "ecs/components/TriggerComponent.hpp"
#include "ecs/components/TagComponent.hpp"
#include "ecs/components/EventInboxComponent.hpp"
#include "ecs/components/BlackboardComponent.hpp"
#include "modules/interaction/InteractionSystem.hpp"
#include "modules/interaction/InteractionNodes.hpp"
#include "modules/ai/GraphContext.hpp"

#include <cstdio>
#include <cmath>
using namespace engine;

extern ::engine::Registry* g_currentRegistryForEmit;

static bool feq(float a,float b,float e=1e-3f){ return std::fabs(a-b)<e; }

struct BoolHolder : GameplayNode {
    bool v=false;
    bool getBool() const override { return v; }
    void execute(float) override {}
    std::string typeName() const override { return "BoolHolder"; }
    std::unique_ptr<GameplayNode> clone() const override { auto up=std::make_unique<BoolHolder>(); up->v=v; return up; }
};

static void check(bool cond, const char* msg, int& fail){
    if(!cond){ printf("FAIL %s\n", msg); fail=1; }
}

int main(){
    int fail=0;
    // 1) Enter / Stay (no spam) / Exit / Re-enter + tag filtering
    {
        Registry reg;
        Entity trig = reg.createEntity();
        TriggerComponent t; t.radius=1.5f; t.targetTag="Player"; t.onEnterEvent="OnEnter"; t.onExitEvent="OnExit";
        reg.addComponent<TriggerComponent>(trig, t);
        TransformComponent tt; tt.position=glm::vec3(0,0,0);
        reg.addComponent<TransformComponent>(trig, tt);
        Entity player = reg.createEntity();
        TransformComponent pt; pt.position=glm::vec3(10,0,0);
        reg.addComponent<TransformComponent>(player, pt);
        reg.addComponent<TagComponent>(player, TagComponent{"Player"});
        // far -> no event
        Engine::InteractionSystem sys;
        sys.update(reg);
        auto* inbox = reg.tryGetComponent<EventInboxComponent>(player);
        if(inbox && !inbox->incomingEvents.empty()) check(false,"far should be no event",fail);
        // move into radius -> Enter
        reg.getComponent<TransformComponent>(player).position = glm::vec3(1,0,0);
        sys.update(reg);
        inbox = reg.tryGetComponent<EventInboxComponent>(player);
        if(!inbox || inbox->incomingEvents.size()!=1) check(false,"enter missing",fail);
        else {
            if(inbox->incomingEvents[0].name!="OnEnter") check(false,"enter name",fail);
            if(inbox->incomingEvents[0].instigatorEntity != (uint32_t)trig) check(false,"enter instigator",fail);
        }
        // stay second frame -> should be 0 events (Stay = silent)
        sys.update(reg);
        inbox = reg.tryGetComponent<EventInboxComponent>(player);
        if(inbox && !inbox->incomingEvents.empty()) check(false,"stay should not spam Enter",fail);
        // move out -> Exit
        reg.getComponent<TransformComponent>(player).position = glm::vec3(10,0,0);
        sys.update(reg);
        inbox = reg.tryGetComponent<EventInboxComponent>(player);
        if(!inbox || inbox->incomingEvents.size()!=1) check(false,"exit missing",fail);
        else if(inbox->incomingEvents[0].name!="OnExit") check(false,"exit name",fail);
        // stay out -> no spam
        sys.update(reg);
        inbox = reg.tryGetComponent<EventInboxComponent>(player);
        if(inbox && !inbox->incomingEvents.empty()) check(false,"outside stay should be no event",fail);
        // re-enter
        reg.getComponent<TransformComponent>(player).position = glm::vec3(0.5f,0,0);
        sys.update(reg);
        inbox = reg.tryGetComponent<EventInboxComponent>(player);
        if(!inbox || inbox->incomingEvents.size()!=1 || inbox->incomingEvents[0].name!="OnEnter") check(false,"re-enter",fail);
        // tag filtering: non-Player should not trigger
        Entity enemy = reg.createEntity();
        TransformComponent et; et.position=glm::vec3(0.2f,0,0);
        reg.addComponent<TransformComponent>(enemy, et);
        reg.addComponent<TagComponent>(enemy, TagComponent{"Enemy"});
        sys.update(reg);
        auto* eInbox = reg.tryGetComponent<EventInboxComponent>(enemy);
        if(eInbox && !eInbox->incomingEvents.empty()) check(false,"enemy tag should not trigger",fail);
        // destroyed entity cleanup: destroy player, ensure no crash and overlaps cleaned
        reg.destroyEntity(player);
        sys.update(reg);
        auto& trigComp = reg.getComponent<TriggerComponent>(trig);
        if(trigComp.currentOverlaps.find((uint32_t)player)!=trigComp.currentOverlaps.end()) check(false,"overlap not cleaned after destroy",fail);
        printf("phase1 Enter/Stay/Exit + tag + destroy OK\n");
    }
    // 2) Multi-entity isolation: 2 triggers, 2 players, each trigger targets different tag
    {
        Registry reg;
        Engine::InteractionSystem sys;
        Entity trigA = reg.createEntity();
        TriggerComponent ta; ta.radius=2.0f; ta.targetTag="Player"; ta.onEnterEvent="A_Enter"; ta.onExitEvent="A_Exit";
        reg.addComponent<TriggerComponent>(trigA, ta);
        TransformComponent tta; tta.position=glm::vec3(0,0,0);
        reg.addComponent<TransformComponent>(trigA, tta);
        Entity trigB = reg.createEntity();
        TriggerComponent tb; tb.radius=2.0f; tb.targetTag="NPC"; tb.onEnterEvent="B_Enter"; tb.onExitEvent="B_Exit";
        reg.addComponent<TriggerComponent>(trigB, tb);
        TransformComponent ttb; ttb.position=glm::vec3(20,0,0);
        reg.addComponent<TransformComponent>(trigB, ttb);
        Entity p1 = reg.createEntity();
        reg.addComponent<TransformComponent>(p1, TransformComponent{glm::vec3(0.5f,0,0)});
        reg.addComponent<TagComponent>(p1, TagComponent{"Player"});
        Entity n1 = reg.createEntity();
        reg.addComponent<TransformComponent>(n1, TransformComponent{glm::vec3(20.5f,0,0)});
        reg.addComponent<TagComponent>(n1, TagComponent{"NPC"});
        sys.update(reg);
        auto* pInbox = reg.tryGetComponent<EventInboxComponent>(p1);
        auto* nInbox = reg.tryGetComponent<EventInboxComponent>(n1);
        if(!pInbox || pInbox->incomingEvents.size()!=1 || pInbox->incomingEvents[0].name!="A_Enter") check(false,"multi A enter",fail);
        if(!nInbox || nInbox->incomingEvents.size()!=1 || nInbox->incomingEvents[0].name!="B_Enter") check(false,"multi B enter",fail);
        // move p1 out, n1 stays -> only p1 gets exit
        reg.getComponent<TransformComponent>(p1).position = glm::vec3(10,0,0);
        sys.update(reg);
        pInbox = reg.tryGetComponent<EventInboxComponent>(p1);
        nInbox = reg.tryGetComponent<EventInboxComponent>(n1);
        if(!pInbox || pInbox->incomingEvents.size()!=1 || pInbox->incomingEvents[0].name!="A_Exit") check(false,"multi A exit",fail);
        if(nInbox && !nInbox->incomingEvents.empty()) check(false,"multi B should stay silent",fail);
        printf("phase2 multi-entity isolation OK\n");
    }
    // 3) Graph reactions: OnEventNode
    {
        Registry reg;
        Entity e = reg.createEntity();
        reg.addComponent<EventInboxComponent>(e, EventInboxComponent{});
        reg.addComponent<BlackboardComponent>(e, BlackboardComponent{});
        AnimParams params;
        GraphContext ctx;
        ctx.selfEntity=(uint32_t)e;
        ctx.incomingEvents = &reg.getComponent<EventInboxComponent>(e).incomingEvents;
        ctx.registry=&reg;
        ctx.dt=0.016f;
        Engine::OnEventNode on;
        on.eventName="OnEnter";
        // no event -> false
        on.execute(ctx, params);
        if(on.getBool()!=false) check(false,"OnEvent false without event",fail);
        // push event -> true + instigator
        reg.getComponent<EventInboxComponent>(e).post(Event{"OnEnter", 99, (uint32_t)e, 1.0f});
        on.execute(ctx, params);
        if(!on.getBool() || on.getInstigator()!=99) check(false,"OnEvent true after event",fail);
        if(!feq(on.getFloat(),1.0f)) check(false,"OnEvent value",fail);
        // next frame clear -> false again (use InteractionSystem clear semantics)
        reg.getComponent<EventInboxComponent>(e).clear();
        on.execute(ctx, params);
        if(on.getBool()) check(false,"OnEvent should be false after clear",fail);
        // wrong name should not trigger
        reg.getComponent<EventInboxComponent>(e).post(Event{"Other", 1, (uint32_t)e, 1.0f});
        on.execute(ctx, params);
        if(on.getBool()) check(false,"OnEvent wrong name",fail);
        printf("phase3 OnEventNode OK\n");
    }
    // 4) EmitEventNode rising edge + modes, and ModifyBlackboardOnEventNode
    {
        Registry reg;
        g_currentRegistryForEmit = &reg;
        Entity attacker = reg.createEntity();
        reg.addComponent<TagComponent>(attacker, TagComponent{"Attacker"});
        Entity victim = reg.createEntity();
        reg.addComponent<EventInboxComponent>(victim, EventInboxComponent{});
        reg.addComponent<BlackboardComponent>(victim, BlackboardComponent{});
        reg.getComponent<BlackboardComponent>(victim).setFloat("Health", 100.0f);
        // Emit graph on attacker
        AnimParams ap;
        BoolHolder* trigger = nullptr;
        Engine::EmitEventNode* emit = nullptr;
        auto g = std::make_shared<GameplayGraph>();
        g->target=&ap;
        trigger = g->addNode<BoolHolder>();
        emit = g->addNode<Engine::EmitEventNode>();
        emit->triggerNode = trigger;
        emit->eventName="Attack";
        emit->targetMode = Engine::EmitTargetMode::Target;
        emit->value=25.0f;
        // context for attacker targeting victim
        GraphContext ctx;
        ctx.selfEntity=(uint32_t)attacker;
        ctx.targetEntity=(uint32_t)victim;
        ctx.registry=&reg;
        ctx.dt=0.016f;
        // trigger false -> no emit
        trigger->v=false;
        emit->execute(ctx, ap);
        if(!reg.getComponent<EventInboxComponent>(victim).incomingEvents.empty()) check(false,"emit should not fire on false",fail);
        // rising true -> emit once
        trigger->v=true;
        emit->execute(ctx, ap);
        if(reg.getComponent<EventInboxComponent>(victim).incomingEvents.size()!=1) check(false,"emit rising once",fail);
        // hold true -> no spam
        emit->execute(ctx, ap);
        if(reg.getComponent<EventInboxComponent>(victim).incomingEvents.size()!=1) check(false,"emit should not spam on hold",fail);
        // victim consumes via ModifyBlackboardOnEventNode
        GraphContext vctx;
        vctx.selfEntity=(uint32_t)victim;
        vctx.incomingEvents=&reg.getComponent<EventInboxComponent>(victim).incomingEvents;
        vctx.blackboard=&reg.getComponent<BlackboardComponent>(victim);
        vctx.registry=&reg;
        vctx.dt=0.016f;
        Engine::ModifyBlackboardOnEventNode mod;
        mod.eventName="Attack";
        mod.targetBlackboardKey="Health";
        mod.deltaValue=-10.0f;
        mod.execute(vctx, ap);
        if(!feq(reg.getComponent<BlackboardComponent>(victim).getFloat("Health"), 90.0f)) check(false,"health after 1st attack 90",fail);
        // next frame: InteractionSystem would clear inbox, but we simulate clear before next emit
        reg.getComponent<EventInboxComponent>(victim).clear();
        // attacker holds trigger still true -> no emit
        emit->execute(ctx, ap);
        vctx.incomingEvents=&reg.getComponent<EventInboxComponent>(victim).incomingEvents;
        mod.execute(vctx, ap);
        if(!feq(reg.getComponent<BlackboardComponent>(victim).getFloat("Health"), 90.0f)) check(false,"health should stay 90 on hold",fail);
        // release and re-trigger
        trigger->v=false;
        emit->execute(ctx, ap); // updates prevTriggered false
        reg.getComponent<EventInboxComponent>(victim).clear();
        trigger->v=true;
        emit->execute(ctx, ap);
        if(reg.getComponent<EventInboxComponent>(victim).incomingEvents.size()!=1) check(false,"emit re-trigger",fail);
        vctx.incomingEvents=&reg.getComponent<EventInboxComponent>(victim).incomingEvents;
        mod.execute(vctx, ap);
        if(!feq(reg.getComponent<BlackboardComponent>(victim).getFloat("Health"), 80.0f)) check(false,"health after 2nd attack 80",fail);
        // Self mode
        reg.getComponent<EventInboxComponent>(victim).clear();
        emit->targetMode = Engine::EmitTargetMode::Self;
        trigger->v=false; emit->execute(ctx, ap);
        trigger->v=true; emit->execute(ctx, ap);
        auto* selfInbox = reg.tryGetComponent<EventInboxComponent>(attacker);
        // attacker had no inbox before, should be auto-created
        if(!selfInbox || selfInbox->incomingEvents.empty() || selfInbox->incomingEvents[0].name!="Attack") check(false,"self mode inbox",fail);
        // Instigator mode (alias to Self for now)
        g_currentRegistryForEmit=nullptr;
        printf("phase4 Emit/Modify rising edge + attack flow OK\n");
    }
    // 5) Full Integration: Interaction -> Gameplay -> Modify (end-to-end)
    {
        Registry reg;
        g_currentRegistryForEmit=&reg;
        Engine::InteractionSystem sys;
        Entity triggerEnt = reg.createEntity();
        TriggerComponent tc; tc.radius=1.5f; tc.targetTag="Player"; tc.onEnterEvent="OnEnter"; tc.onExitEvent="OnExit";
        reg.addComponent<TriggerComponent>(triggerEnt, tc);
        reg.addComponent<TransformComponent>(triggerEnt, TransformComponent{glm::vec3(0,0,0)});
        Entity player = reg.createEntity();
        reg.addComponent<TransformComponent>(player, TransformComponent{glm::vec3(10,0,0)});
        reg.addComponent<TagComponent>(player, TagComponent{"Player"});
        reg.addComponent<EventInboxComponent>(player, EventInboxComponent{});
        reg.addComponent<BlackboardComponent>(player, BlackboardComponent{});
        reg.getComponent<BlackboardComponent>(player).setFloat("Score", 0.0f);
        // player graph: OnEnter -> Modify Score +5
        AnimParams pp;
        auto pg = std::make_shared<GameplayGraph>();
        pg->target=&pp;
        auto* on = pg->addNode<Engine::OnEventNode>(); on->eventName="OnEnter";
        // chain via Modify that listens same event (simplified: directly use Modify node)
        auto* mod = pg->addNode<Engine::ModifyBlackboardOnEventNode>(); mod->eventName="OnEnter"; mod->targetBlackboardKey="Score"; mod->deltaValue=5.0f;
        // 1st frame far
        sys.update(reg);
        {
            GraphContext ctx; ctx.selfEntity=(uint32_t)player; ctx.incomingEvents=&reg.getComponent<EventInboxComponent>(player).incomingEvents; ctx.blackboard=&reg.getComponent<BlackboardComponent>(player); ctx.registry=&reg; ctx.dt=0.016f;
            pg->execute(ctx, pp); // Should not modify
        }
        if(!feq(reg.getComponent<BlackboardComponent>(player).getFloat("Score"),0.0f)) check(false,"score far 0",fail);
        // move into trigger
        reg.getComponent<TransformComponent>(player).position = glm::vec3(0.5f,0,0);
        sys.update(reg);
        {
            GraphContext ctx; ctx.selfEntity=(uint32_t)player; ctx.incomingEvents=&reg.getComponent<EventInboxComponent>(player).incomingEvents; ctx.blackboard=&reg.getComponent<BlackboardComponent>(player); ctx.registry=&reg; ctx.dt=0.016f;
            pg->execute(ctx, pp);
        }
        if(!feq(reg.getComponent<BlackboardComponent>(player).getFloat("Score"),5.0f)) check(false,"score after enter 5",fail);
        // stay -> sys clears previous OnEnter, no new event -> score stays 5
        sys.update(reg);
        {
            GraphContext ctx; ctx.selfEntity=(uint32_t)player; ctx.incomingEvents=&reg.getComponent<EventInboxComponent>(player).incomingEvents; ctx.blackboard=&reg.getComponent<BlackboardComponent>(player); ctx.registry=&reg; ctx.dt=0.016f;
            pg->execute(ctx, pp);
        }
        if(!feq(reg.getComponent<BlackboardComponent>(player).getFloat("Score"),5.0f)) check(false,"score stay 5",fail);
        // exit -> OnExit event, but our mod listens OnEnter only, so score stays 5
        reg.getComponent<TransformComponent>(player).position = glm::vec3(10,0,0);
        sys.update(reg);
        {
            GraphContext ctx; ctx.selfEntity=(uint32_t)player; ctx.incomingEvents=&reg.getComponent<EventInboxComponent>(player).incomingEvents; ctx.blackboard=&reg.getComponent<BlackboardComponent>(player); ctx.registry=&reg; ctx.dt=0.016f;
            pg->execute(ctx, pp);
            if(!reg.getComponent<EventInboxComponent>(player).incomingEvents.empty()){
                if(reg.getComponent<EventInboxComponent>(player).incomingEvents[0].name!="OnExit") check(false,"expected OnExit",fail);
            } else check(false,"missing OnExit",fail);
        }
        if(!feq(reg.getComponent<BlackboardComponent>(player).getFloat("Score"),5.0f)) check(false,"score after exit stay 5",fail);
        g_currentRegistryForEmit=nullptr;
        printf("phase5 integration Interaction->Gameplay->Blackboard OK\n");
    }

    if(fail) { printf("FAIL interaction_system_test\n"); return 1; }
    printf("PASS: interaction Enter/Stay/Exit, multi-entity, OnEvent/Emit/Modify, attack flow\n");
    return 0;
}
