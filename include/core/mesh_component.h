#pragma once

namespace engine {

struct Mesh;

struct MeshComponent {
    Mesh* mesh = nullptr; // shared, owned externally (asset store / primitives)
};

} // namespace engine
