#include "core/scene/SceneSerializer.hpp"
#include "core/scene/Scene.hpp"
#include "core/scene.h"
#include "ecs/components/TagComponent.hpp"
#include "core/transform_component.h"
#include "ecs/components/PhysicsComponent.hpp"
#include "ecs/components/ColliderComponent.hpp"
#include "ecs/components/BlackboardComponent.hpp"
#include "ecs/components/PathComponent.hpp"
#include "core/gameplay_component.h"
#include "core/gameplay_graph.h"

#include <cstdio>
#include <cmath>
#include <filesystem>
#include <fstream>

using namespace engine;
namespace fs = std::filesystem;

static bool feq(float a,float b,float eps=1e-4f){ return std::fabs(a-b)<eps; }
static bool veq(const glm::vec3& a,const glm::vec3& b,float eps=1e-4f){ return feq(a.x,b.x,eps) && feq(a.y,b.y,eps) && feq(a.z,b.z,eps); }

int main(){
    // Test 1: Roundtrip save -> load -> verify
    {
        Registry reg;
        // Create Player
        Entity player = reg.createEntity();
        reg.addComponent<TagComponent>(player, TagComponent("Player"));
        TransformComponent pt; pt.position = glm::vec3(1,2,3); pt.rotation = glm::vec3(0.1f,0.2f,0.3f); pt.scale = glm::vec3(1,1,1);
        reg.addComponent<TransformComponent>(player, pt);
        PhysicsComponent pphys; pphys.mass=2.0f; pphys.linearDamping=5.0f; pphys.useGravity=false; pphys.velocity=glm::vec3(0.5f,0,0.5f);
        reg.addComponent<PhysicsComponent>(player, pphys);
        ColliderComponent col; col.type=ColliderType::Sphere; col.radius=0.7f; col.halfExtents=glm::vec3(0.7f,1,0.7f);
        reg.addComponent<ColliderComponent>(player, col);
        BlackboardComponent bb; bb.setFloat("ChaseRadius", 6.0f); bb.setBool("IsAlert", true); bb.setVec3("LastSeen", glm::vec3(1,2,3));
        reg.addComponent<BlackboardComponent>(player, bb);
        PathComponent path; path.waypoints = {glm::vec3(0,0,0), glm::vec3(1,0,1)}; path.hasPath=true; path.currentIndex=1; path.destination=glm::vec3(1,0,1);
        reg.addComponent<PathComponent>(player, path);
        GameplayComponent gc; gc.graph = GameplayGraph::makeMinimal(nullptr);
        reg.addComponent<GameplayComponent>(player, std::move(gc));

        // Create Guard
        Entity guard = reg.createEntity();
        reg.addComponent<TagComponent>(guard, TagComponent("Guard_NPC"));
        TransformComponent gt; gt.position = glm::vec3(8,0,8); gt.rotation=glm::vec3(0,0,0); gt.scale=glm::vec3(1,1,1);
        reg.addComponent<TransformComponent>(guard, gt);
        PhysicsComponent gphys; gphys.mass=1.0f; gphys.linearDamping=10.0f; gphys.useGravity=true;
        reg.addComponent<PhysicsComponent>(guard, gphys);
        ColliderComponent gcol; gcol.type=ColliderType::Sphere; gcol.radius=0.5f;
        reg.addComponent<ColliderComponent>(guard, gcol);
        BlackboardComponent gbb; gbb.setFloat("ChaseRadius", 6.0f); gbb.setFloat("PatrolSpeed", 1.5f); gbb.setBool("IsAlert", false);
        reg.addComponent<BlackboardComponent>(guard, gbb);
        PathComponent gpath; gpath.clear();
        reg.addComponent<PathComponent>(guard, gpath);
        GameplayComponent ggc; ggc.graph = GameplayGraph::makeBlackboardChase(nullptr, 6.0f, 2.2f, 3.0f);
        reg.addComponent<GameplayComponent>(guard, std::move(ggc));
        // Add targetTag via blackboard for guard -> player
        {
            auto* bb2 = reg.tryGetComponent<BlackboardComponent>(guard);
            bb2->setFloat("TargetEntityID", static_cast<float>(static_cast<uint32_t>(player)));
        }

        fs::path tmp = fs::temp_directory_path() / "test_scene_roundtrip.json";
        if (!Engine::SceneSerializer::serialize(tmp, reg)) { printf("FAIL roundtrip serialize\n"); return 1; }
        // Verify file exists and contains expected tags
        {
            std::ifstream in(tmp);
            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (content.find("Player") == std::string::npos || content.find("Guard_NPC") == std::string::npos) {
                printf("FAIL serialized missing tags\n"); return 1;
            }
        }
        // Wipe and reload
        reg.clear();
        if (reg.getAllEntities().size() != 0) { printf("FAIL clear not empty\n"); return 1; }
        if (!Engine::SceneSerializer::deserialize(tmp, reg)) { printf("FAIL roundtrip deserialize\n"); return 1; }
        auto entities = reg.getAllEntities();
        if (entities.size() != 2) { printf("FAIL roundtrip entity count %zu\n", entities.size()); return 1; }
        // Find by tag
        Entity foundPlayer = kInvalidEntity, foundGuard = kInvalidEntity;
        for (auto e : entities) {
            auto* tag = reg.tryGetComponent<TagComponent>(e);
            if (!tag) { printf("FAIL missing tag\n"); return 1; }
            if (tag->tag == "Player") foundPlayer = e;
            else if (tag->tag == "Guard_NPC") foundGuard = e;
        }
        if (foundPlayer == kInvalidEntity || foundGuard == kInvalidEntity) { printf("FAIL tags not found after load\n"); return 1; }
        // Verify transform
        {
            auto* tr = reg.tryGetComponent<TransformComponent>(foundPlayer);
            if (!tr || !veq(tr->position, glm::vec3(1,2,3))) { printf("FAIL player pos %.2f %.2f %.2f\n", tr ? tr->position.x : 0, tr ? tr->position.y : 0, tr ? tr->position.z : 0); return 1; }
            if (!feq(tr->rotation.x, 0.1f) || !feq(tr->rotation.y, 0.2f) || !feq(tr->rotation.z, 0.3f)) { printf("FAIL player rot %.2f %.2f %.2f\n", tr->rotation.x, tr->rotation.y, tr->rotation.z); return 1; }
        }
        {
            auto* phys = reg.tryGetComponent<PhysicsComponent>(foundPlayer);
            if (!phys || !feq(phys->mass, 2.0f) || !feq(phys->linearDamping, 5.0f) || phys->useGravity) { printf("FAIL player physics\n"); return 1; }
        }
        {
            auto* col = reg.tryGetComponent<ColliderComponent>(foundGuard);
            if (!col || col->type != ColliderType::Sphere || !feq(col->radius, 0.5f)) { printf("FAIL guard collider\n"); return 1; }
        }
        {
            auto* bb = reg.tryGetComponent<BlackboardComponent>(foundGuard);
            if (!bb || !feq(bb->getFloat("ChaseRadius",0), 6.0f) || !feq(bb->getFloat("PatrolSpeed",0), 1.5f) || bb->getBool("IsAlert", true)) { printf("FAIL guard blackboard\n"); return 1; }
        }
        {
            auto* p = reg.tryGetComponent<PathComponent>(foundPlayer);
            if (!p || p->waypoints.size()!=2 || !p->hasPath || p->currentIndex!=1) { printf("FAIL player path\n"); return 1; }
        }
        {
            auto* gc2 = reg.tryGetComponent<GameplayComponent>(foundGuard);
            if (!gc2 || !gc2->graph) { printf("FAIL guard gameplay missing\n"); return 1; }
            // Check target linking: guard's blackboard should have TargetEntityID == player
            auto* bb = reg.tryGetComponent<BlackboardComponent>(foundGuard);
            float tid = bb ? bb->getFloat("TargetEntityID", -1) : -1;
            if (static_cast<uint32_t>(tid) != static_cast<uint32_t>(foundPlayer)) {
                printf("FAIL guard targetTag not linked correctly got %.0f expected %u\n", tid, foundPlayer);
                return 1;
            }
        }
        // Also verify vector blackboard roundtrip
        {
            auto* bb = reg.tryGetComponent<BlackboardComponent>(foundPlayer);
            if (!bb || !veq(bb->getVec3("LastSeen", glm::vec3(0)), glm::vec3(1,2,3))) { printf("FAIL player vec3\n"); return 1; }
        }
        fs::remove(tmp);
        printf("PASS roundtrip\n");
    }

    // Test 2: Invalid JSON recovery
    {
        Registry reg;
        fs::path bad = fs::temp_directory_path() / "bad_scene.json";
        {
            std::ofstream out(bad);
            out << "{ invalid json [ }";
        }
        bool ok = Engine::SceneSerializer::deserialize(bad, reg);
        if (ok) { printf("FAIL invalid JSON should return false\n"); return 1; }
        if (!reg.getAllEntities().empty()) { printf("FAIL invalid JSON should not create entities\n"); return 1; }
        fs::remove(bad);
        // Also test missing file
        bool ok2 = Engine::SceneSerializer::deserialize("nonexistent_path_12345.json", reg);
        if (ok2) { printf("FAIL missing file should return false\n"); return 1; }
        printf("PASS invalid JSON recovery\n");
    }

    // Test 3: Missing reference handling (orphaned targetTag)
    {
        Registry reg;
        fs::path tmp = fs::temp_directory_path() / "orphan_scene.json";
        // Create a scene JSON with a Guard that references non-existent targetTag
        {
            std::ofstream out(tmp);
            out << R"({
  "scene": "Orphan Test",
  "version": 1,
  "entities": [
    {
      "tag": "Guard_NPC",
      "transform": {"position":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
      "gameplay": {"graphTemplate":"", "targetTag":"NonExistentPlayer"}
    }
  ]
})";
        }
        bool ok = Engine::SceneSerializer::deserialize(tmp, reg);
        if (!ok) { printf("FAIL orphan deserialize should succeed with warning\n"); return 1; }
        auto entities = reg.getAllEntities();
        if (entities.size()!=1) { printf("FAIL orphan entity count %zu\n", entities.size()); return 1; }
        Entity e = entities[0];
        auto* bb = reg.tryGetComponent<BlackboardComponent>(e);
        if (!bb) { printf("FAIL orphan blackboard missing\n"); return 1; }
        float tid = bb->getFloat("TargetEntityID", -999);
        if (!feq(tid, 0.0f)) { printf("FAIL orphan should set 0 got %.1f\n", tid); return 1; }
        // Also check that graph fallback was assigned (empty graph)
        auto* gc = reg.tryGetComponent<GameplayComponent>(e);
        if (!gc || !gc->graph) { printf("FAIL orphan gameplay fallback missing\n"); return 1; }
        fs::remove(tmp);
        printf("PASS missing reference handling\n");
    }

    // Test 4: Missing asset path fallback (graphTemplate not found)
    {
        Registry reg;
        fs::path tmp = fs::temp_directory_path() / "missing_asset.json";
        {
            std::ofstream out(tmp);
            out << R"({
  "scene": "Missing Asset",
  "version": 1,
  "entities": [
    {
      "tag": "Player",
      "transform": {"position":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
      "gameplay": {"graphTemplate":"assets/graphs/does_not_exist_123.json"}
    }
  ]
})";
        }
        bool ok = Engine::SceneSerializer::deserialize(tmp, reg);
        if (!ok) { printf("FAIL missing asset deserialize should succeed with fallback\n"); return 1; }
        auto entities = reg.getAllEntities();
        if (entities.empty()) { printf("FAIL missing asset no entity\n"); return 1; }
        auto* gc = reg.tryGetComponent<GameplayComponent>(entities[0]);
        if (!gc || !gc->graph) { printf("FAIL missing asset fallback graph missing\n"); return 1; }
        // Should be fallback minimal graph, at least not crash when executed
        // Try to execute it
        try {
            gc->graph->execute(0.016f, *(new AnimParams()));
        } catch(...) { printf("FAIL fallback graph execute threw\n"); return 1; }
        fs::remove(tmp);
        printf("PASS missing asset fallback\n");
    }

    // Test 5: Demo world file exists and loads
    {
        Registry reg;
        bool ok = Engine::SceneSerializer::deserialize("assets/scenes/demo_world.scene.json", reg);
        if (!ok) {
            // Try alternative path
            ok = Engine::SceneSerializer::deserialize("build/assets/scenes/demo_world.scene.json", reg);
        }
        if (!ok) { printf("FAIL demo_world load\n"); return 1; }
        auto entities = reg.getAllEntities();
        if (entities.size() < 2) { printf("FAIL demo_world entities %zu\n", entities.size()); return 1; }
        bool hasPlayer=false, hasGuard=false;
        for(auto e: entities){
            auto* tag = reg.tryGetComponent<TagComponent>(e);
            if(tag && tag->tag=="Player") hasPlayer=true;
            if(tag && tag->tag=="Guard_NPC") hasGuard=true;
        }
        if(!hasPlayer || !hasGuard){ printf("FAIL demo_world missing tags player %d guard %d\n", hasPlayer, hasGuard); return 1; }
        // Check that Guard has gameplay and path
        for(auto e: entities){
            auto* tag = reg.tryGetComponent<TagComponent>(e);
            if(tag && tag->tag=="Guard_NPC"){
                auto* gc = reg.tryGetComponent<GameplayComponent>(e);
                auto* path = reg.tryGetComponent<PathComponent>(e);
                auto* bb = reg.tryGetComponent<BlackboardComponent>(e);
                if(!gc || !gc->graph){ printf("FAIL demo_world guard no graph\n"); return 1; }
                if(!path){ printf("FAIL demo_world guard no path\n"); return 1; }
                if(!bb){ printf("FAIL demo_world guard no bb\n"); return 1; }
                // Check blackboard values from demo_world
                if(!feq(bb->getFloat("ChaseRadius",0), 6.0f)){ printf("FAIL demo_world ChaseRadius %.1f\n", bb->getFloat("ChaseRadius",0)); return 1; }
            }
        }
        printf("PASS demo_world load\n");
    }

    printf("PASS: scene serialization all tests\n");
    return 0;
}
