#pragma once
#include <glm/glm.hpp>
#include <vector>

namespace Engine {

struct GridCoord {
    int x{0};
    int z{0};
    bool operator==(const GridCoord& o) const { return x == o.x && z == o.z; }
};

class NavGrid {
public:
    NavGrid(int width, int height, float cellSize, glm::vec3 origin = glm::vec3(0.0f))
        : width_(width), height_(height), cellSize_(cellSize), origin_(origin),
          walkable_(width * height, true) {}

    bool isWalkable(int x, int z) const {
        if (x < 0 || x >= width_ || z < 0 || z >= height_) return false;
        return walkable_[z * width_ + x];
    }

    void setWalkable(int x, int z, bool walkable) {
        if (x >= 0 && x < width_ && z >= 0 && z < height_) {
            walkable_[z * width_ + x] = walkable;
        }
    }

    GridCoord worldToGrid(const glm::vec3& worldPos) const {
        int gx = static_cast<int>((worldPos.x - origin_.x) / cellSize_);
        int gz = static_cast<int>((worldPos.z - origin_.z) / cellSize_);
        return {gx, gz};
    }

    glm::vec3 gridToWorld(int x, int z) const {
        return glm::vec3(
            origin_.x + (x + 0.5f) * cellSize_,
            origin_.y,
            origin_.z + (z + 0.5f) * cellSize_
        );
    }

    int getWidth() const { return width_; }
    int getHeight() const { return height_; }
    float getCellSize() const { return cellSize_; }
    glm::vec3 getOrigin() const { return origin_; }

private:
    int width_{0};
    int height_{0};
    float cellSize_{1.0f};
    glm::vec3 origin_{0.0f};
    std::vector<bool> walkable_;
};

} // namespace Engine

namespace engine {
    using NavGrid = ::Engine::NavGrid;
    using GridCoord = ::Engine::GridCoord;
}
