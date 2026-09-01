#include "renderer/meshlet/MeshletPipeline.hpp"
#include "renderer/graph/RenderGraphBuilder.hpp"
#include "renderer/deferred/GBuffer.hpp"
#include "renderer/material/BindlessDescriptorManager.hpp"

namespace Engine {

void MeshletPipeline::init(VkDevice device, VkPhysicalDevice physicalDevice) {
    device_ = device;
    physicalDevice_ = physicalDevice;
    // In real engine, create compute pipeline for meshlet_cull.comp and graphics pipeline for gbuffer_meshlet.vert
    // For headless, keep null
}

void MeshletPipeline::shutdown() {
    if (device_ != VK_NULL_HANDLE) {
        if (meshletCullPipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, meshletCullPipeline_, nullptr);
        if (gbufferMeshletPipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, gbufferMeshletPipeline_, nullptr);
    }
    meshletCullPipeline_ = VK_NULL_HANDLE;
    gbufferMeshletPipeline_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    physicalDevice_ = VK_NULL_HANDLE;
}

void MeshletPipeline::buildPipeline(RenderGraph& graph,
                                     GBufferHandles& gbuffer,
                                     ResourceHandle hizPyramid,
                                     BufferHandle allMeshlets,
                                     BufferHandle compactedIndices,
                                     BufferHandle indirectCmd) {
    // GBuffer handles may be uninitialized if caller hasn't declared; declare if needed
    // But spec expects gbuffer to be passed in already declared. We'll just use as is.

    // 1. Meshlet Compute Culling & Index Compaction Pass
    graph.addPass("Meshlet_Cull_And_Compact", QueueType::AsyncCompute,
        [&](RenderGraphBuilder& b) {
            b.read(hizPyramid, ResourceUsage::ShaderRead);
            b.read(allMeshlets, BufferUsage::ComputeRead);
            b.write(compactedIndices, BufferUsage::ComputeWrite);
            b.write(indirectCmd, BufferUsage::ComputeWrite);
        },
        [&](VkCommandBuffer cb) {
            // Zero out DrawIndexedIndirectCommand.indexCount
            if (cb != VK_NULL_HANDLE && physicalIndirectBuffer_ != VK_NULL_HANDLE) {
                vkCmdFillBuffer(cb, physicalIndirectBuffer_, 0, sizeof(uint32_t), 0);
                // Barrier would be needed between fill and dispatch, but omitted for headless
            }
            if (cb != VK_NULL_HANDLE && meshletCullPipeline_ != VK_NULL_HANDLE) {
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, meshletCullPipeline_);
                uint32_t groupCount = (totalMeshlets_ + 63) / 64;
                vkCmdDispatch(cb, groupCount, 1, 1);
            }
            (void)cb;
        }
    );

    // 2. G-Buffer Rasterization Pass (Executes single consolidated draw)
    graph.addPass("GBuffer_Meshlet_Draw", QueueType::Graphics,
        [&](RenderGraphBuilder& b) {
            b.read(compactedIndices, BufferUsage::IndexBuffer);
            b.read(indirectCmd, BufferUsage::IndirectBuffer);
            b.write(gbuffer.albedoAO, ResourceUsage::ColorAttachment);
            b.write(gbuffer.normalRoughness, ResourceUsage::ColorAttachment);
            b.write(gbuffer.metallicFlags, ResourceUsage::ColorAttachment);
            b.write(gbuffer.depth, ResourceUsage::DepthStencilAttachment);
        },
        [&](VkCommandBuffer cb) {
            if (cb != VK_NULL_HANDLE && gbufferMeshletPipeline_ != VK_NULL_HANDLE) {
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gbufferMeshletPipeline_);
            }
            // Bind global bindless set
            // In real engine: VkDescriptorSet globalSet = bindlessManager.getDescriptorSet();
            // vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gbufferLayout, 0, 1, &globalSet, 0, nullptr);
            // Bind dynamically compacted index buffer
            if (cb != VK_NULL_HANDLE && physicalCompactedIndexBuffer_ != VK_NULL_HANDLE) {
                vkCmdBindIndexBuffer(cb, physicalCompactedIndexBuffer_, 0, VK_INDEX_TYPE_UINT32);
                // Issue consolidated draw driven by GPU compute compaction counter
                vkCmdDrawIndexedIndirect(cb, physicalIndirectBuffer_, 0, 1, sizeof(VkDrawIndexedIndirectCommand));
            }
            (void)cb;
        }
    );
}

} // namespace Engine
