#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace engine {

// CPU-side vertex. Layout matches pipeline vertex input:
// location 0 = position (vec3), location 1 = normal (vec3), location 2 = uv (vec2),
// location 9 = jointIndices (vec4), location 10 = jointWeights (vec4).
struct Vertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    glm::vec2 uv{0.0f};
    glm::vec4 jointIndices{0.0f}; // x,y,z,w = joint indices as floats (cast to int in shader)
    glm::vec4 jointWeights{0.0f}; // x,y,z,w = weights, sum should be 1.0 for skinned verts
};

struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    bool empty() const { return vertices.empty() || indices.empty(); }
};

// Built-in primitives (CPU-side until asset loading exists).
namespace mesh_primitives {

Mesh triangle();
Mesh quad(); // two triangles, proves index buffer path

} // namespace mesh_primitives

} // namespace engine
