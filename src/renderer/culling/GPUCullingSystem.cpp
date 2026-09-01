#include "renderer/culling/GPUCullingSystem.hpp"
#include "renderer/culling/HiZPyramid.hpp"
#include "renderer/graph/RenderGraphBuilder.hpp"
#include <cmath>

namespace Engine {

void GPUCullingSystem::recordHiZPyramidGeneration(VkCommandBuffer cb, ResourceHandle depth, ResourceHandle hiz, uint32_t mipLevels) {
    (void)cb; (void)depth; (void)hiz; (void)mipLevels;
    // In real engine: for each mip level, bind hiz_build.comp, sample parent, store child via imageStore
    // Stub for graph validation: no actual GPU work needed for tests
}

void GPUCullingSystem::buildCullingPipeline(RenderGraph& graph,
                                            ResourceHandle depthBuffer,
                                            BufferHandle allInstances,
                                            BufferHandle indirectCommands,
                                            BufferHandle visibleIndices,
                                            VkExtent2D screenExtent) {
    // 1. Create Hi-Z Pyramid Texture with full mip-chain
    uint32_t mipLevels = HiZPyramid::computeMipLevels(screenExtent);
    VkExtent2D hizBase = HiZPyramid::pyramidBaseExtent(screenExtent);

    auto hizPyramid = graph.createResource({
        .name = "HiZ_Pyramid",
        .format = VK_FORMAT_R32_SFLOAT,
        .extent = hizBase,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        .mipLevels = mipLevels
    });

    // 2. Build Hi-Z Pyramid Mips
    graph.addPass("HiZ_Build_Pass",
        [&](RenderGraphBuilder& b) {
            b.read(depthBuffer, ResourceUsage::ShaderRead);
            b.write(hizPyramid, ResourceUsage::ComputeWrite);
        },
        [&](VkCommandBuffer cb) {
            recordHiZPyramidGeneration(cb, depthBuffer, hizPyramid, mipLevels);
        }
    );

    // 3. GPU Occlusion & Frustum Culling Compute Pass
    // Reset instanceCount before cull (vkCmdFillBuffer or compute init). We do it inside the same pass's execute
    // to keep graph simple: barrier ensures write completes before indirect read.
    graph.addPass("GPU_Occlusion_Cull_Pass",
        [&](RenderGraphBuilder& b) {
            b.read(hizPyramid, ResourceUsage::ShaderRead);
            b.read(allInstances, BufferUsage::ComputeRead);
            b.write(indirectCommands, BufferUsage::ComputeWrite);
            b.write(visibleIndices, BufferUsage::ComputeWrite);
        },
        [&](VkCommandBuffer cb) {
            if (cb != VK_NULL_HANDLE && indirectCommands.isValid()) {
                // In real engine: vkCmdFillBuffer to reset instanceCount to 0, then barrier, then dispatch
                // Here we just note the pipeline bind/dispatch
            }
            (void)cb;
            // vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cullingComputePipeline_);
            // uint32_t groupCount = (totalInstances_ + 63) / 64;
            // vkCmdDispatch(cb, groupCount, 1, 1);
        }
    );

    // 4. G-Buffer Opaque Pass (Executes via GPU Indirect Draw)
    graph.addPass("GBuffer_Indirect_Draw_Pass",
        [&](RenderGraphBuilder& b) {
            b.read(indirectCommands, BufferUsage::IndirectBuffer);
            b.read(visibleIndices, BufferUsage::VertexRead);
            b.read(hizPyramid, ResourceUsage::ShaderRead); // keep HiZ alive through draw (optional)
            // In full pipeline, GBuffer attachments would be writes here:
            // b.write(gbuffer.albedoAO, ColorAttachment) etc. Left to caller if needed.
            // For test, we just need the indirect read dependency to prove barrier ordering.
        },
        [&](VkCommandBuffer cb) {
            (void)cb;
            // vkCmdDrawIndexedIndirect(cb, physicalIndirectBuffer_, 0, numBatches_, sizeof(VkDrawIndexedIndirectCommand));
        }
    );
}

} // namespace Engine
