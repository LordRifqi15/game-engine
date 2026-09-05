#include "renderer/deferred/GBuffer.hpp"
#include "renderer/graph/RenderGraph.hpp"

namespace Engine {

GBufferHandles GBuffer::declare(RenderGraph& graph, VkExtent2D extent) {
    GBufferHandles handles;
    handles.albedoAO = graph.createResource({
        .name = "GBuffer_AlbedoAO",
        .format = VK_FORMAT_R8G8B8A8_SRGB,
        .extent = extent,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
    });
    handles.normalRoughness = graph.createResource({
        .name = "GBuffer_NormalRoughness",
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .extent = extent,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
    });
    handles.metallicFlags = graph.createResource({
        .name = "GBuffer_MetallicFlags",
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = extent,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
    });
    handles.depth = graph.createResource({
        .name = "SceneDepth",
        .format = VK_FORMAT_D32_SFLOAT,
        .extent = extent,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT
    });
    return handles;
}

} // namespace Engine
