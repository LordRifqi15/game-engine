#pragma once
#include "renderer/meshlet/MeshletTypes.hpp"
#include <glm/glm.hpp>
#include <vector>
#include <cstdint>

namespace Engine {

class MeshletBuilder {
public:
    // Computes normal cone axis and cutoff sine from triangle normals
    static glm::vec4 computeNormalCone(const std::vector<glm::vec3>& triangleNormals);

    // Computes bounding sphere for a set of positions (Ritter's minimal)
    static glm::vec4 computeBoundingSphere(const std::vector<glm::vec3>& positions, const std::vector<uint32_t>& uniqueIndices);

    // Subdivides mesh index buffer into clusters under 64 vertices / 124 triangles
    // outUniqueVertexIndices: global indirection buffer (unique vertices per meshlet, concatenated)
    // outPackedLocalIndices: packed 3*u8 per triangle, concatenated
    static std::vector<GPUMeshlet> buildMeshlets(
        const std::vector<glm::vec3>& positions,
        const std::vector<uint32_t>& indices,
        std::vector<uint32_t>& outUniqueVertexIndices,
        std::vector<uint8_t>& outPackedLocalIndices
    );

    // Helper for tests: build from positions + indices with normals
    static std::vector<GPUMeshlet> buildMeshletsWithNormals(
        const std::vector<glm::vec3>& positions,
        const std::vector<glm::vec3>& normals,
        const std::vector<uint32_t>& indices,
        std::vector<uint32_t>& outUniqueVertexIndices,
        std::vector<uint8_t>& outPackedLocalIndices
    );
};

} // namespace Engine

namespace engine {
    using MeshletBuilder = ::Engine::MeshletBuilder;
}
