#pragma once
#include "renderer/graph/RenderGraphResources.hpp"
#include <vector>
#include <cstdint>

namespace Engine {

struct ResourceLifetime {
    ResourceHandle handle;
    uint32_t firstPass{UINT32_MAX};
    uint32_t lastPass{0};

    bool isAliveDuring(uint32_t passIndex) const {
        return passIndex >= firstPass && passIndex <= lastPass;
    }

    bool overlaps(const ResourceLifetime& other) const {
        return !(lastPass < other.firstPass || firstPass > other.lastPass);
    }
};

} // namespace Engine

namespace engine {
    using ResourceLifetime = ::Engine::ResourceLifetime;
}
