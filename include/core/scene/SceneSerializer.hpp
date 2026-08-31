#pragma once
#include <filesystem>
#include <string>

// Forward declare engine types to avoid heavy includes
namespace engine {
    class Registry;
    class Scene;
}

namespace Engine {

class SceneSerializer {
public:
    // Core API operating on Registry (custom engine registry)
    static bool serialize(const std::filesystem::path& filepath, ::engine::Registry& registry);
    static bool deserialize(const std::filesystem::path& filepath, ::engine::Registry& registry);

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
