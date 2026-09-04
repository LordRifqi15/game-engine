#pragma once
#include "renderer/meshlet/MeshletTypes.hpp"
#include "renderer/graph/RenderGraph.hpp"
#include "renderer/graph/RenderGraphResources.hpp"
#include <vulkan/vulkan.h>

namespace Engine {

struct GBufferHandles;

class MeshletPipeline {
public:
    MeshletPipeline() = default;

    void init(VkDevice device, VkPhysicalDevice physicalDevice);
    void shutdown();

    // Build meshlet culling + compacted draw passes
    void buildPipeline(RenderGraph& graph,
                       GBufferHandles& gbuffer,
                       ResourceHandle hizPyramid,
                       BufferHandle allMeshlets,
                       BufferHandle compactedIndices,
                       BufferHandle indirectCmd);

    // For tests: set total meshlet count for dispatch
    void setTotalMeshlets(uint32_t count) { totalMeshlets_ = count; }
    uint32_t totalMeshlets() const { return totalMeshlets_; }

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
    VkPipeline meshletCullPipeline_{VK_NULL_HANDLE};
    VkPipeline gbufferMeshletPipeline_{VK_NULL_HANDLE};
    VkBuffer physicalCompactedIndexBuffer_{VK_NULL_HANDLE};
    VkBuffer physicalIndirectBuffer_{VK_NULL_HANDLE};
    uint32_t totalMeshlets_{0};
};

} // namespace Engine

namespace engine {
    using MeshletPipeline = ::Engine::MeshletPipeline;
}
