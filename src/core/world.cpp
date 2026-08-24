#include "core/world.h"

namespace engine {

World::World(Registry& registry, Mesh* groundMesh, int loadRadius, float chunkSize)
    : registry_(registry), groundMesh_(groundMesh), loadRadius_(loadRadius),
      chunkSize_(chunkSize) {}

World::~World() {
    for (auto& [coord, chunk] : chunks_) {
        for (Entity e : chunk.entities) registry_.destroyEntity(e);
    }
}

void World::update(const glm::vec3& cameraPos) {
    // Camera's current chunk.
    int32_t camX = static_cast<int32_t>(std::floor(cameraPos.x / chunkSize_));
    int32_t camZ = static_cast<int32_t>(std::floor(cameraPos.z / chunkSize_));

    // Unload chunks outside the radius.
    std::vector<ChunkCoord> toUnload;
    for (const auto& [coord, chunk] : chunks_) {
        if (std::abs(coord.x - camX) > loadRadius_ ||
            std::abs(coord.z - camZ) > loadRadius_) {
            toUnload.push_back(coord);
        }
    }
    for (const auto& coord : toUnload) unloadChunk(coord);

    // Load missing chunks within radius.
    for (int dx = -loadRadius_; dx <= loadRadius_; ++dx) {
        for (int dz = -loadRadius_; dz <= loadRadius_; ++dz) {
            ChunkCoord coord{camX + dx, camZ + dz};
            if (!chunks_.count(coord)) loadChunk(coord);
        }
    }
}

void World::loadChunk(ChunkCoord coord) {
    auto& chunk = chunks_[coord];

    // One triangle per chunk center.
    Entity e = registry_.createEntity();
    TransformComponent t;
    t.position = {coord.x * chunkSize_ + chunkSize_ * 0.5f, 0.0f,
                  coord.z * chunkSize_ + chunkSize_ * 0.5f};
    registry_.addComponent<TransformComponent>(e, t);
    // Mesh assignment: caller sets via setMesh(e) or we use a default.
    // For now, store entity and let Engine assign mesh.
    chunk.entities.push_back(e);
}

void World::unloadChunk(const ChunkCoord& coord) {
    auto it = chunks_.find(coord);
    if (it == chunks_.end()) return;
    for (Entity e : it->second.entities) registry_.destroyEntity(e);
    chunks_.erase(it);
}

size_t World::totalEntities() const {
    return chunks_.size();
}

} // namespace engine
