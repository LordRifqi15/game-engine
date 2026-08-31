#include "modules/navigation/NavGrid.hpp"
#include "modules/navigation/Pathfinding.hpp"
#include "modules/navigation/NavigationNodes.hpp"
#include "ecs/components/PathComponent.hpp"
#include "ecs/components/PhysicsComponent.hpp"
#include "modules/ai/GraphContext.hpp"
#include "core/gameplay_graph.h"

#include <cstdio>
#include <cmath>

using namespace engine;

static bool feq(float a,float b,float eps=1e-3f){ return std::fabs(a-b)<eps; }
static bool veq(const glm::vec3& a,const glm::vec3& b,float eps=1e-3f){ return feq(a.x,b.x,eps)&&feq(a.y,b.y,eps)&&feq(a.z,b.z,eps); }

int main(){
    // 1) Coordinate conversions
    {
        NavGrid grid(10,10,1.0f, glm::vec3(0,0,0));
        auto gc = grid.worldToGrid(glm::vec3(0.2f,0,0.7f));
        if (gc.x!=0 || gc.z!=0){ printf("FAIL worldToGrid origin\n"); return 1; }
        auto gc2 = grid.worldToGrid(glm::vec3(5.6f,0,3.2f));
        if (gc2.x!=5 || gc2.z!=3){ printf("FAIL worldToGrid 5.6,3.2 got %d,%d\n", gc2.x,gc2.z); return 1; }
        auto w = grid.gridToWorld(2,3);
        if (!feq(w.x, 2.5f) || !feq(w.z, 3.5f)){ printf("FAIL gridToWorld 2,3 got %.2f,%.2f\n", w.x,w.z); return 1; }
        // With origin offset
        NavGrid grid2(8,8,2.0f, glm::vec3(-4,0,-4));
        auto gc3 = grid2.worldToGrid(glm::vec3(0,0,0));
        if (gc3.x!=2 || gc3.z!=2){ printf("FAIL origin worldToGrid got %d,%d\n", gc3.x,gc3.z); return 1; }
        auto w2 = grid2.gridToWorld(2,2);
        if (!feq(w2.x, 1.0f) || !feq(w2.z, 1.0f)){ printf("FAIL origin gridToWorld %.2f,%.2f\n", w2.x,w2.z); return 1; }
    }
    // 2) Walkable checks and out of bounds
    {
        NavGrid grid(5,5,1.0f);
        grid.setWalkable(2,2,false);
        if (grid.isWalkable(2,2)) { printf("FAIL setWalkable false\n"); return 1; }
        if (!grid.isWalkable(0,0)) { printf("FAIL walkable true\n"); return 1; }
        if (grid.isWalkable(-1,0) || grid.isWalkable(5,0) || grid.isWalkable(0,5)) { printf("FAIL out of bounds should false\n"); return 1; }
        grid.setWalkable(-1,0,false); // should not crash
        grid.setWalkable(10,10,false);
    }
    // 3) A* straight line
    {
        NavGrid grid(10,10,1.0f);
        auto path = Pathfinding::findPath(grid, glm::vec3(0.5f,0,0.5f), glm::vec3(9.5f,0,0.5f));
        if (path.empty()){ printf("FAIL straight line empty\n"); return 1; }
        // Should be roughly 10 waypoints
        if (path.size() < 9 || path.size() > 11){ printf("FAIL straight line size %zu\n", path.size()); return 1; }
        // First and last should be near start and target
        if (!veq(path.front(), glm::vec3(0.5f,0,0.5f), 0.1f)) { printf("FAIL straight start %.2f,%.2f\n", path.front().x, path.front().z); return 1; }
        if (!veq(path.back(), glm::vec3(9.5f,0,0.5f), 0.1f)) { printf("FAIL straight end %.2f,%.2f\n", path.back().x, path.back().z); return 1; }
    }
    // 4) Obstacle avoidance: wall
    {
        NavGrid grid(10,10,1.0f);
        // Vertical wall at x=5 from z=2 to z=7
        for(int z=2;z<=7;++z) grid.setWalkable(5,z,false);
        auto path = Pathfinding::findPath(grid, glm::vec3(2.5f,0,5.5f), glm::vec3(7.5f,0,5.5f));
        if (path.empty()){ printf("FAIL wall path empty\n"); return 1; }
        // Path should go around wall, so it must not contain walkable false cells and should be longer than straight
        for(auto& wp: path){
            auto gc = grid.worldToGrid(wp);
            if (!grid.isWalkable(gc.x, gc.z)){ printf("FAIL wall path contains blocked %d,%d\n", gc.x,gc.z); return 1; }
        }
        // Straight distance is 5, wall should cause detour >5
        if (path.size() <= 5){ printf("FAIL wall path not detour size %zu\n", path.size()); return 1; }
    }
    // 5) U-shaped barrier
    {
        NavGrid grid(10,10,1.0f);
        // U shape: bottom at z=5 x=3-6, sides at x=3 and x=6 z=5-8, open at top
        for(int x=3;x<=6;++x) grid.setWalkable(x,5,false);
        for(int z=5;z<=8;++z){ grid.setWalkable(3,z,false); grid.setWalkable(6,z,false); }
        // Start inside U at (4.5,0,6.5) target outside at (4.5,0,9.5) - must go around
        auto path = Pathfinding::findPath(grid, glm::vec3(4.5f,0,6.5f), glm::vec3(4.5f,0,9.5f));
        if (path.empty()){ printf("FAIL U path empty\n"); return 1; }
        for(auto& wp: path){
            auto gc = grid.worldToGrid(wp);
            if (!grid.isWalkable(gc.x, gc.z)){ printf("FAIL U path blocked %d,%d\n", gc.x,gc.z); return 1; }
        }
        // Path should be longer than direct 3 cells
        if (path.size() <= 3){ printf("FAIL U path not around size %zu\n", path.size()); return 1; }
    }
    // 6) Unreachable and nearest walkable
    {
        NavGrid grid(5,5,1.0f);
        // Block target cell
        grid.setWalkable(4,4,false);
        auto path = Pathfinding::findPath(grid, glm::vec3(0.5f,0,0.5f), glm::vec3(4.5f,0,4.5f));
        // Should find nearest walkable to target, not empty
        if (path.empty()){ printf("FAIL nearest walkable empty\n"); return 1; }
        auto last = path.back();
        auto gc = grid.worldToGrid(last);
        if (!grid.isWalkable(gc.x, gc.z)){ printf("FAIL nearest not walkable %d,%d\n", gc.x,gc.z); return 1; }
        // Fully blocked: surround start with walls
        NavGrid grid2(5,5,1.0f);
        for(int x=0;x<5;++x) for(int z=0;z<5;++z) grid2.setWalkable(x,z,false);
        grid2.setWalkable(2,2,true); // only start walkable
        auto path2 = Pathfinding::findPath(grid2, glm::vec3(2.5f,0,2.5f), glm::vec3(4.5f,0,4.5f));
        // No path to target, should return empty or just start?
        // Our impl returns empty if no path found (since target not walkable and no alternative)
        // For this fully blocked case, it should be empty or single
        // We check that it doesn't crash and is either empty or size 1
        if (!path2.empty() && path2.size()!=1){ printf("FAIL blocked path size %zu\n", path2.size()); return 1; }
    }
    // 7) Diagonal with corner check: diagonal only if orthogonal neighbors walkable
    {
        NavGrid grid(5,5,1.0f);
        grid.setWalkable(1,0,false);
        grid.setWalkable(0,1,false);
        // Start (0,0) to (1,1) diagonal should be blocked because (1,0) and (0,1) blocked
        auto path = Pathfinding::findPath(grid, glm::vec3(0.5f,0,0.5f), glm::vec3(1.5f,0,1.5f));
        // Path should go around, not direct diagonal
        if (path.size() == 2 && veq(path[0], glm::vec3(0.5f,0,0.5f)) && veq(path[1], glm::vec3(1.5f,0,1.5f))){
            printf("FAIL diagonal corner not blocked\n"); return 1;
        }
    }
    // 8) RequestPathNode and FollowPathNode integration
    {
        NavGrid grid(10,10,1.0f);
        PathComponent pathComp;
        PhysicsComponent phys; phys.velocity=glm::vec3(0); phys.linearDamping=0;
        BlackboardComponent bb;
        // RequestPath
        RequestPathNode req;
        req.targetPos = glm::vec3(5.5f,0,5.5f);
        req.triggerValue = true;
        GraphContext ctx{};
        ctx.selfPosition = glm::vec3(0.5f,0,0.5f);
        ctx.targetPosition = glm::vec3(5.5f,0,5.5f);
        ctx.path = &pathComp;
        ctx.navGrid = &grid;
        ctx.dt = 0.016f;
        AnimParams p;
        req.execute(ctx, p);
        if (!pathComp.hasPath || pathComp.waypoints.empty()){ printf("FAIL RequestPath not hasPath\n"); return 1; }
        if (pathComp.waypoints.size() < 5){ printf("FAIL RequestPath waypoints %zu\n", pathComp.waypoints.size()); return 1; }
        // FollowPath should set velocity towards first waypoint
        FollowPathNode follow;
        follow.speedValue = 2.0f;
        follow.acceptanceRadius = 0.3f;
        follow.enabledValue = true;
        ctx.selfPosition = glm::vec3(0.5f,0,0.5f);
        ctx.outPhysics = &phys;
        follow.execute(ctx, p);
        if (feq(phys.velocity.x, 0.0f) && feq(phys.velocity.z, 0.0f)){ printf("FAIL FollowPath vel 0\n"); return 1; }
        // Simulate reaching first waypoint
        ctx.selfPosition = pathComp.waypoints[0];
        // Set selfPosition to exactly first waypoint, should advance
        size_t beforeIdx = pathComp.currentIndex;
        follow.execute(ctx, p);
        if (pathComp.currentIndex != beforeIdx+1){ printf("FAIL FollowPath not advanced %zu -> %zu\n", beforeIdx, pathComp.currentIndex); return 1; }
        // Simulate arrival at final waypoint
        pathComp.currentIndex = pathComp.waypoints.size()-1;
        ctx.selfPosition = pathComp.waypoints.back();
        follow.execute(ctx, p);
        if (!pathComp.isFinished || pathComp.hasPath){ printf("FAIL FollowPath not finished hasPath %d isFinished %d\n", pathComp.hasPath, pathComp.isFinished); return 1; }
        if (!follow.getBool()){ printf("FAIL FollowPath hasArrived false\n"); return 1; }
    }
    // 9) FollowPath with disabled should not set velocity
    {
        NavGrid grid(5,5,1.0f);
        PathComponent pathComp;
        pathComp.waypoints = {glm::vec3(1.5f,0,0.5f), glm::vec3(2.5f,0,0.5f)};
        pathComp.hasPath = true; pathComp.currentIndex=0;
        PhysicsComponent phys; phys.velocity=glm::vec3(1,0,0);
        FollowPathNode follow;
        follow.enabledValue = false;
        GraphContext ctx{};
        ctx.selfPosition=glm::vec3(0.5f,0,0.5f);
        ctx.path=&pathComp; ctx.navGrid=&grid; ctx.outPhysics=&phys; ctx.dt=0.016f;
        AnimParams p;
        follow.execute(ctx,p);
        if (!feq(phys.velocity.x, 1.0f) || !feq(phys.velocity.z, 0.0f)){ printf("FAIL Follow disabled should not change vel %.2f,%.2f\n", phys.velocity.x, phys.velocity.z); return 1; }
        if (follow.getBool()){ printf("FAIL Follow disabled hasArrived should false\n"); return 1; }
    }
    // 10) Cloning for navigation nodes
    {
        AnimParams p1,p2;
        auto g1 = std::make_shared<GameplayGraph>();
        auto* req = g1->addNode<RequestPathNode>(); req->targetPos=glm::vec3(5,0,5); req->triggerValue=true;
        auto* follow = g1->addNode<FollowPathNode>(); follow->speedValue=2.0f; follow->enabledValue=true;
        // Link not needed for clone test, just check clone copies
        auto g2 = g1->clone(&p2);
        if (g2->nodes.size()!=g1->nodes.size()){ printf("FAIL nav clone size\n"); return 1; }
        auto* req2 = dynamic_cast<RequestPathNode*>(g2->nodes[0].get());
        auto* follow2 = dynamic_cast<FollowPathNode*>(g2->nodes[1].get());
        if (!req2 || !follow2){ printf("FAIL nav clone types\n"); return 1; }
        if (!veq(req2->targetPos, glm::vec3(5,0,5))){ printf("FAIL nav clone req target\n"); return 1; }
        if (!feq(follow2->speedValue, 2.0f)){ printf("FAIL nav clone follow speed\n"); return 1; }
        // Ensure distinct instances
        if (req2==req || follow2==follow){ printf("FAIL nav clone same instance\n"); return 1; }
    }

    printf("PASS: navigation grid, A*, obstacles, path following, cloning\n");
    return 0;
}
