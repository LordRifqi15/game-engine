#pragma once
#include "renderer/graph/RenderGraphResources.hpp"
#include <vulkan/vulkan.h>

namespace Engine {

struct BarrierState {
    VkPipelineStageFlags stageMask;
    VkAccessFlags accessMask;
    VkImageLayout layout;
};

BarrierState getBarrierState(ResourceUsage usage);
BarrierState getBarrierState(BufferUsage usage);
VkImageAspectFlags getAspectMask(VkFormat format, ResourceUsage usage);
VkImageAspectFlags getAspectMask(const RenderGraphResource& res, ResourceUsage usage);

} // namespace Engine

namespace engine {
    using BarrierState = Engine::BarrierState;
}
