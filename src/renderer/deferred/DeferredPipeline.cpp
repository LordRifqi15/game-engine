#include "renderer/deferred/DeferredPipeline.hpp"
#include "renderer/graph/RenderGraphBuilder.hpp"
#include "renderer/deferred/GBuffer.hpp"
#include "renderer/lighting/ClusteredLighting.hpp"
#include "renderer/lighting/LightTypes.hpp"
#include "core/registry.h"

namespace Engine {

void DeferredPipeline::buildPipeline(RenderGraph& graph, ::engine::Registry& registry, ResourceHandle swapchainTarget, VkExtent2D extent) {
    (void)registry;
    // 1. Declare G-Buffer Transient Attachments
    GBufferHandles gbuffer = GBuffer::declare(graph, extent);

    auto hdrTarget = graph.createResource({
        .name = "HDR_Color",
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .extent = extent,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
    });

    auto shadowMap = graph.createResource({
        .name = "ShadowMap",
        .format = VK_FORMAT_D32_SFLOAT,
        .extent = {2048, 2048},
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
    });

    // 2. Shadow Pass
    graph.addPass("ShadowPass",
        [&](RenderGraphBuilder& b) {
            b.write(shadowMap, ResourceUsage::DepthStencilAttachment);
        },
        [&](VkCommandBuffer cb) { recordShadows(cb, registry); }
    );

    // 3. G-Buffer Opaque Pass (MRT)
    graph.addPass("GBufferPass",
        [&](RenderGraphBuilder& b) {
            b.write(gbuffer.albedoAO, ResourceUsage::ColorAttachment);
            b.write(gbuffer.normalRoughness, ResourceUsage::ColorAttachment);
            b.write(gbuffer.metallicFlags, ResourceUsage::ColorAttachment);
            b.write(gbuffer.depth, ResourceUsage::DepthStencilAttachment);
        },
        [&](VkCommandBuffer cb) { recordGBufferGeometry(cb, registry); }
    );

    // 3b. Clustered Light Buffers (GPU)
    uint32_t gridX = ClusteredLighting::computeGridX(extent);
    uint32_t gridY = ClusteredLighting::computeGridY(extent);
    uint32_t totalClusters = gridX * gridY * ClusteredLighting::CLUSTER_SLICES_Z;
    auto lightBuffer = graph.createBuffer({
        .name = "ClusterLights",
        .size = size_t(ClusteredLighting::MAX_LIGHTS) * sizeof(GPULight),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
    });
    auto clusterGridBuffer = graph.createBuffer({
        .name = "ClusterGrid",
        .size = size_t(totalClusters) * sizeof(ClusterCell),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
    });
    auto clusterIndexBuffer = graph.createBuffer({
        .name = "ClusterIndices",
        .size = sizeof(uint32_t) + size_t(totalClusters) * ClusteredLighting::MAX_LIGHTS_PER_CLUSTER * sizeof(uint32_t),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
    });

    // 3c. Cluster Cull Compute Pass
    graph.addPass("ClusterCull",
        [&](RenderGraphBuilder& b) {
            b.read(lightBuffer, BufferUsage::ComputeRead);
            b.write(clusterGridBuffer, BufferUsage::ComputeWrite);
            b.write(clusterIndexBuffer, BufferUsage::ComputeWrite);
        },
        [&](VkCommandBuffer cb) {
            (void)cb;
            // In real engine: bind pipeline cluster_cull.comp, push constants gridDim, dispatch totalClusters/64
            // Stub for graph validation: no actual dispatch needed
        }
    );

    // 4. Deferred Lighting Pass (now clustered)
    graph.addPass("DeferredLightingPass",
        [&](RenderGraphBuilder& b) {
            b.read(gbuffer.albedoAO, ResourceUsage::ShaderRead);
            b.read(gbuffer.normalRoughness, ResourceUsage::ShaderRead);
            b.read(gbuffer.metallicFlags, ResourceUsage::ShaderRead);
            b.read(gbuffer.depth, ResourceUsage::ShaderRead);
            b.read(shadowMap, ResourceUsage::ShaderRead);
            b.read(lightBuffer, BufferUsage::FragmentRead);
            b.read(clusterGridBuffer, BufferUsage::FragmentRead);
            b.read(clusterIndexBuffer, BufferUsage::FragmentRead);
            b.write(hdrTarget, ResourceUsage::ColorAttachment);
        },
        [&](VkCommandBuffer cb) { recordLightingQuad(cb, registry); }
    );

    // 5. Forward Transparency & Skybox Pass (Reuses Depth Read-Only)
    graph.addPass("ForwardOverlayPass",
        [&](RenderGraphBuilder& b) {
            b.read(gbuffer.depth, ResourceUsage::DepthStencilAttachment); // Read-only depth testing
            b.write(hdrTarget, ResourceUsage::ColorAttachment);
        },
        [&](VkCommandBuffer cb) { recordForwardTransparents(cb, registry); }
    );

    // 6. Tonemapping / Post-Processing Pass
    graph.addPass("PostProcessPass",
        [&](RenderGraphBuilder& b) {
            b.read(hdrTarget, ResourceUsage::ShaderRead);
            b.write(swapchainTarget, ResourceUsage::ColorAttachment);
        },
        [&](VkCommandBuffer cb) { recordTonemapping(cb); }
    );
}

void DeferredPipeline::recordShadows(VkCommandBuffer cb, ::engine::Registry& reg) { (void)cb; (void)reg; }
void DeferredPipeline::recordGBufferGeometry(VkCommandBuffer cb, ::engine::Registry& reg) { (void)cb; (void)reg; }
void DeferredPipeline::recordLightingQuad(VkCommandBuffer cb, ::engine::Registry& reg) { (void)cb; (void)reg; }
void DeferredPipeline::recordForwardTransparents(VkCommandBuffer cb, ::engine::Registry& reg) { (void)cb; (void)reg; }
void DeferredPipeline::recordTonemapping(VkCommandBuffer cb) { (void)cb; }

} // namespace Engine
