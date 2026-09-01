#pragma once
#include "renderer/graph/RenderGraph.hpp"
#include "renderer/graph/RenderGraphResources.hpp"
#include <vulkan/vulkan.h>

namespace Engine {

class GPUCullingSystem {
public:
    GPUCullingSystem() = default;

    // Build Hi-Z pyramid + culling + indirect draw passes.
    // Caller provides already-created buffers (or transient handles) and depth buffer.
    void buildCullingPipeline(RenderGraph& graph,
                              ResourceHandle depthBuffer,
                              BufferHandle allInstances,
                              BufferHandle indirectCommands,
                              BufferHandle visibleIndices,
                              VkExtent2D screenExtent);

    // Optional: set runtime params (total instances, batches). If not set, pipeline uses defaults.
    void setInstanceCount(uint32_t count) { totalInstances_ = count; }
    void setBatchCount(uint32_t batches) { numBatches_ = batches; }
    void setIndirectBuffer(VkBuffer buf) { physicalIndirectBuffer_ = buf; }

private:
    void recordHiZPyramidGeneration(VkCommandBuffer cb, ResourceHandle depth, ResourceHandle hiz, uint32_t mipLevels);

    VkPipeline cullingComputePipeline_{VK_NULL_HANDLE};
    VkBuffer physicalIndirectBuffer_{VK_NULL_HANDLE};
    uint32_t totalInstances_{0};
    uint32_t numBatches_{1};
};

} // namespace Engine

namespace engine {
    using GPUCullingSystem = Engine::GPUCullingSystem;
}
