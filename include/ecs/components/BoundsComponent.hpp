#pragma once
#include <glm/glm.hpp>
#include <cstdint>

namespace Engine {

// Local-space bounding volume for GPU culling. Holds both sphere and AABB
// so frustum (sphere) and Hi-Z (AABB projection) tests can run without conversion.
struct BoundsComponent {
    glm::vec3 center{0.0f};      // local-space offset from entity origin
    float radius{1.0f};          // bounding sphere radius

    glm::vec3 aabbMin{-0.5f};    // local-space AABB min
    glm::vec3 aabbMax{0.5f};     // local-space AABB max

    // Convenience: build from sphere center/radius, AABB derived as sphere extents
    static BoundsComponent fromSphere(const glm::vec3& c, float r) {
        BoundsComponent b;
        b.center = c;
        b.radius = r;
        b.aabbMin = c - glm::vec3(r);
        b.aabbMax = c + glm::vec3(r);
        return b;
    }
    static BoundsComponent fromAABB(const glm::vec3& mn, const glm::vec3& mx) {
        BoundsComponent b;
        b.aabbMin = mn;
        b.aabbMax = mx;
        b.center = (mn + mx) * 0.5f;
        b.radius = glm::length(mx - b.center);
        return b;
    }
};

// Separate named components for ECS that prefers split storage (optional)
struct BoundingSphereComponent {
    glm::vec3 center{0.0f};
    float radius{1.0f};
};

struct AABBComponent {
    glm::vec3 min{-0.5f};
    glm::vec3 max{0.5f};
};

} // namespace Engine

namespace engine {
    using BoundsComponent = ::Engine::BoundsComponent;
    using BoundingSphereComponent = ::Engine::BoundingSphereComponent;
    using AABBComponent = ::Engine::AABBComponent;
}
