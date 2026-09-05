#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "core/mesh.h"

// Forward declare engine types to avoid heavy includes
namespace engine {
    class Registry;
    class Scene;
    class TextureCache;
}

namespace Engine {

class SceneSerializer {
public:
    // Asset-aware scene content. meshes points at engine-owned storage
    // (reserve upfront; MeshComponents point into it). Null assets (or null
    // meshes) = skip mesh/material/light keys.
    struct SceneAssets {
        std::vector<::engine::Mesh>* meshes = nullptr;
        ::engine::TextureCache* textures = nullptr;
    };
    // Core API operating on Registry (custom engine registry)
    static bool serialize(const std::filesystem::path& filepath, ::engine::Registry& registry);
    static bool deserialize(const std::filesystem::path& filepath, ::engine::Registry& registry,
                            SceneAssets* assets = nullptr);

    // Convenience overloads operating on Scene wrapper
    static bool serialize(const std::filesystem::path& filepath, ::engine::Scene& scene);
    static bool deserialize(const std::filesystem::path& filepath, ::engine::Scene& scene);

    // Compatibility overloads for task spec that mentions entt::registry
    // If entt is not available, these will be aliases to engine::Registry
    // We forward declare entt::registry as engine::Registry for compilation
};

} // namespace Engine

// Provide lowercase alias for engine namespace (project uses lowercase)
namespace engine {
    using SceneSerializer = ::Engine::SceneSerializer;
}

// Provide entt::registry alias if entt not present (task spec uses entt::registry)
#ifndef ENTT_ENTT_HPP
namespace entt {
    using registry = ::engine::Registry;
}
#endif
