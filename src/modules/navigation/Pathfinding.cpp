#include "modules/navigation/Pathfinding.hpp"
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <cmath>
#include <algorithm>

namespace Engine {

struct GridCoordHash {
    size_t operator()(const GridCoord& c) const noexcept {
        return (static_cast<size_t>(c.x) * 73856093u) ^ (static_cast<size_t>(c.z) * 19349663u);
    }
};

static float heuristic(const GridCoord& a, const GridCoord& b, float cellSize) {
    float dx = static_cast<float>(a.x - b.x) * cellSize;
    float dz = static_cast<float>(a.z - b.z) * cellSize;
    return std::sqrt(dx*dx + dz*dz);
}

static GridCoord findNearestWalkable(const NavGrid& grid, GridCoord c) {
    if (grid.isWalkable(c.x, c.z)) return c;
    // BFS for nearest walkable
    std::queue<GridCoord> q;
    std::unordered_set<GridCoord, GridCoordHash> visited;
    q.push(c);
    visited.insert(c);
    const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
    while (!q.empty()) {
        auto cur = q.front(); q.pop();
        for (auto& d : dirs) {
            GridCoord nb{cur.x + d[0], cur.z + d[1]};
            if (visited.count(nb)) continue;
            visited.insert(nb);
            if (nb.x < 0 || nb.x >= grid.getWidth() || nb.z <0 || nb.z >= grid.getHeight()) continue;
            if (grid.isWalkable(nb.x, nb.z)) return nb;
            q.push(nb);
        }
    }
    return c; // fallback
}

std::vector<glm::vec3> Pathfinding::findPath(const NavGrid& grid, const glm::vec3& startWorld, const glm::vec3& targetWorld) {
    GridCoord start = grid.worldToGrid(startWorld);
    GridCoord target = grid.worldToGrid(targetWorld);

    // Clamp to grid bounds and find nearest walkable for target/start if needed
    auto clampCoord = [&](GridCoord c){
        c.x = std::max(0, std::min(c.x, grid.getWidth()-1));
        c.z = std::max(0, std::min(c.z, grid.getHeight()-1));
        return c;
    };
    start = clampCoord(start);
    target = clampCoord(target);

    // If start or target not walkable, find nearest walkable
    if (!grid.isWalkable(start.x, start.z)) start = findNearestWalkable(grid, start);
    if (!grid.isWalkable(target.x, target.z)) target = findNearestWalkable(grid, target);

    if (!grid.isWalkable(start.x, start.z) || !grid.isWalkable(target.x, target.z)) {
        return {};
    }
    if (start == target) {
        return { grid.gridToWorld(start.x, start.z) };
    }

    struct Node {
        GridCoord coord;
        float g{0}, h{0}, f{0};
        GridCoord parent{-1,-1};
        bool hasParent{false};
    };

    struct PQItem {
        float f;
        GridCoord coord;
        bool operator<(const PQItem& o) const { return f > o.f; } // min-heap
    };

    std::priority_queue<PQItem> open;
    std::unordered_map<GridCoord, Node, GridCoordHash> allNodes;
    std::unordered_map<GridCoord, float, GridCoordHash> gScore;
    std::unordered_set<GridCoord, GridCoordHash> closed;

    Node startNode{start, 0, heuristic(start, target, grid.getCellSize()), 0, {-1,-1}, false};
    startNode.f = startNode.g + startNode.h;
    allNodes[start] = startNode;
    gScore[start] = 0;
    open.push({startNode.f, start});

    const int dirs[8][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};

    while (!open.empty()) {
        GridCoord cur = open.top().coord; open.pop();
        if (closed.count(cur)) continue;
        if (cur == target) {
            // reconstruct
            std::vector<glm::vec3> path;
            GridCoord c = target;
            while (true) {
                auto it = allNodes.find(c);
                if (it == allNodes.end()) break;
                path.push_back(grid.gridToWorld(c.x, c.z));
                if (!it->second.hasParent) break;
                c = it->second.parent;
                if (closed.count(c) == 0 && !(c == start) && allNodes.find(c)==allNodes.end()) break;
            }
            std::reverse(path.begin(), path.end());
            return path;
        }
        closed.insert(cur);
        auto curIt = allNodes.find(cur);
        if (curIt == allNodes.end()) continue;
        float curG = curIt->second.g;

        for (auto& d : dirs) {
            GridCoord nb{cur.x + d[0], cur.z + d[1]};
            if (nb.x <0 || nb.x >= grid.getWidth() || nb.z <0 || nb.z >= grid.getHeight()) continue;
            if (!grid.isWalkable(nb.x, nb.z)) continue;
            // Diagonal check: need orthogonal neighbors walkable
            if (d[0]!=0 && d[1]!=0) {
                if (!grid.isWalkable(cur.x + d[0], cur.z) || !grid.isWalkable(cur.x, cur.z + d[1])) continue;
            }
            if (closed.count(nb)) continue;
            float moveCost = (d[0]==0 || d[1]==0) ? grid.getCellSize() : grid.getCellSize() * 1.41421356f;
            float tentativeG = curG + moveCost;
            auto gIt = gScore.find(nb);
            if (gIt == gScore.end() || tentativeG < gIt->second) {
                gScore[nb] = tentativeG;
                float h = heuristic(nb, target, grid.getCellSize());
                Node n{nb, tentativeG, h, tentativeG + h, cur, true};
                allNodes[nb] = n;
                open.push({n.f, nb});
            }
        }
    }
    return {};
}

} // namespace Engine
