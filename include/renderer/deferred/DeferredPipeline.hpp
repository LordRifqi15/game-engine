#pragma once
#include "renderer/graph/RenderGraph.hpp"
#include "renderer/graph/RenderGraphResources.hpp"
#include "renderer/deferred/GBuffer.hpp"
#include "core/registry.h"
#include <vulkan/vulkan.h>

namespace Engine {

class DeferredPipeline {
public:
    void buildPipeline(RenderGraph& graph, ::engine::Registry& registry, ResourceHandle swapchainTarget, VkExtent2D extent);

private:
    void recordShadows(VkCommandBuffer cb, ::engine::Registry& registry);
    void recordGBufferGeometry(VkCommandBuffer cb, ::engine::Registry& registry);
    void recordLightingQuad(VkCommandBuffer cb, ::engine::Registry& registry);
    void recordForwardTransparents(VkCommandBuffer cb, ::engine::Registry& registry);
    void recordTonemapping(VkCommandBuffer cb);
};


} // namespace Engine

namespace engine {
    using DeferredPipeline = Engine::DeferredPipeline;
}
