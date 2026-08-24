#pragma once

#include "core/material.h"
#include "core/mesh.h"
#include "core/registry.h"
#include "core/transform_component.h"

#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include <unordered_map>
#include <vector>

namespace engine {

struct ChunkCoord {
    int32_t x = 0;
    int32_t z = 0;
    bool operator==(const ChunkCoord& o) const { return x == o.x && z == o.z; }
};

struct ChunkCoordHash {
    size_t operator()(const ChunkCoord& c) const {
        uint64_t h = static_cast<uint64_t>(static_cast<uint32_t>(c.x)) * 0x9E3779B97F4A7C15ull;
        h ^= static_cast<uint64_t>(static_cast<uint32_t>(c.z)) * 0xC2B2AE3D27D4EB4Full;
        return static_cast<size_t>(h);
    }
};

class World {
public:
    World(Registry& registry, Mesh* groundMesh, int loadRadius = 2,
          float chunkSize = 16.0f);
    ~World();

    void update(const glm::vec3& cameraPos);

    size_t loadedChunkCount() const { return chunks_.size(); }
    size_t totalEntities() const;

private:
    void loadChunk(ChunkCoord coord);
    void unloadChunk(const ChunkCoord& coord);

    Registry& registry_;
    Mesh* groundMesh_;

    int loadRadius_;
    float chunkSize_;

    struct LoadedChunk {
        std::vector<Entity> entities;
    };
    std::unordered_map<ChunkCoord, LoadedChunk, ChunkCoordHash> chunks_;
};

} // namespace engine
