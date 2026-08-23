#pragma once

namespace engine {

// Marks entities participating in CPU occlusion culling.
// Optional: entities without it are always drawn.
struct OcclusionComponent {
    bool visibleLastFrame = true;
};

} // namespace engine
