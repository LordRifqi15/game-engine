#pragma once
#include "renderer/graph/RenderGraphResources.hpp"
#include <vulkan/vulkan.h>

namespace Engine { class RenderGraph; }

namespace Engine {

struct GBufferHandles {
    ResourceHandle albedoAO;
    ResourceHandle normalRoughness;
    ResourceHandle metallicFlags;
    ResourceHandle depth;
};

class GBuffer {
public:
    static GBufferHandles declare(RenderGraph& graph, VkExtent2D extent);
};

} // namespace Engine

namespace engine {
    using GBufferHandles = Engine::GBufferHandles;
    using GBuffer = Engine::GBuffer;
}
