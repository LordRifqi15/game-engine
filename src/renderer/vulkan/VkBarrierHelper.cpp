#include "renderer/graph/RenderGraph.hpp"
#include "renderer/api/Synchronization.hpp"
#include "renderer/vulkan/PhysicalImage.hpp"
#include <stdexcept>

namespace Engine {

BarrierState getBarrierState(BufferUsage usage) {
    switch (usage) {
        case BufferUsage::ComputeRead:
            return {VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED};
        case BufferUsage::ComputeWrite:
            return {VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED};
        case BufferUsage::FragmentRead:
            return {VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED};
        case BufferUsage::VertexRead:
            return {VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED};
        case BufferUsage::TransferSrc:
            return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED};
        case BufferUsage::TransferDst:
            return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED};
        case BufferUsage::IndirectBuffer:
            return {VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED};
        case BufferUsage::IndexBuffer:
            return {VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_ACCESS_INDEX_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED};
        case BufferUsage::None:
        default:
            return {VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED};
    }
}

BarrierState getBarrierState(ResourceUsage usage) {
    switch (usage) {
        case ResourceUsage::ColorAttachment:
            return {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        case ResourceUsage::DepthStencilAttachment:
            return {VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        case ResourceUsage::ShaderRead:
            return {VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        case ResourceUsage::ComputeRead:
            return {VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        case ResourceUsage::ComputeWrite:
            return {VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_SHADER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_GENERAL};
        case ResourceUsage::TransferSrc:
            return {VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
        case ResourceUsage::TransferDst:
            return {VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};
        case ResourceUsage::Present:
            return {VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                    0,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
        case ResourceUsage::None:
        default:
            return {VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED};
    }
}

VkImageAspectFlags getAspectMask(VkFormat format, ResourceUsage usage) {
    if (usage == ResourceUsage::DepthStencilAttachment) return VK_IMAGE_ASPECT_DEPTH_BIT;
    // depth format detection
    switch (format) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        default:
            return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

VkImageAspectFlags getAspectMask(const RenderGraphResource& res, ResourceUsage usage) {
    return getAspectMask(res.desc.format, usage);
}

void RenderGraph::resolveBarriers(uint32_t passIndex, VkCommandBuffer cmdBuffer) {
    if (passIndex >= passes_.size()) return;
    const auto& pass = passes_[passIndex];

    auto getStateForLayout = [](VkImageLayout layout)->BarrierState {
        switch (layout) {
            case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
                return {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
                return {VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
                return {VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
            case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                return {VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
                return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
            case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
                return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};
            case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
                return {VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
            case VK_IMAGE_LAYOUT_UNDEFINED:
            default:
                return {VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED};
        }
    };

    auto getStateForUsage = [](ResourceUsage usage, bool isWrite)->BarrierState {
        if (usage == ResourceUsage::DepthStencilAttachment) {
            if (isWrite) {
                return {VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
            } else {
                return {VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
            }
        }
        return getBarrierState(usage);
    };

    auto checkAndAddBarrier = [&](ResourceHandle handle, ResourceUsage currentUsage, bool isWrite) {
        if (!handle.isValid() || handle.id >= resources_.size()) return;
        auto& res = resources_[handle.id];
        BarrierState srcState;
        bool hasPhysical = (res.physicalBinding != nullptr);
        if (hasPhysical) {
            srcState = getStateForLayout(res.physicalBinding->currentLayout);
        } else {
            if (res.lastUsage == currentUsage) return;
            srcState = getBarrierState(res.lastUsage);
        }
        BarrierState dstState = getStateForUsage(currentUsage, isWrite);
        if (srcState.layout == dstState.layout && srcState.accessMask == dstState.accessMask && !hasPhysical) return;
        if (hasPhysical && srcState.layout == dstState.layout) {
            res.lastUsage = currentUsage;
            return;
        }
        if (res.image != VK_NULL_HANDLE && cmdBuffer != VK_NULL_HANDLE) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = srcState.accessMask;
            barrier.dstAccessMask = dstState.accessMask;
            barrier.oldLayout = srcState.layout;
            barrier.newLayout = dstState.layout;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = res.image;
            barrier.subresourceRange.aspectMask = getAspectMask(res, currentUsage);
            barrier.subresourceRange.baseMipLevel = 0;
            barrier.subresourceRange.levelCount = std::max(1u, res.desc.mipLevels);
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(cmdBuffer, srcState.stageMask, dstState.stageMask,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }
        res.lastUsage = currentUsage;
        if (hasPhysical) res.physicalBinding->currentLayout = dstState.layout;
    };

    for (const auto& [handle, usage] : pass.reads) checkAndAddBarrier(handle, usage, false);
    for (const auto& [handle, usage] : pass.writes) checkAndAddBarrier(handle, usage, true);

    // Buffer barriers (Clustered)
    auto checkAndAddBufferBarrier = [&](BufferHandle handle, BufferUsage currentUsage) {
        if (!handle.isValid() || handle.id >= bufferResources_.size()) return;
        auto& res = bufferResources_[handle.id];
        BarrierState srcState = getBarrierState(res.lastUsage);
        BarrierState dstState = getBarrierState(currentUsage);
        if (res.lastUsage == currentUsage) return;
        if (srcState.stageMask == dstState.stageMask && srcState.accessMask == dstState.accessMask) {
            if (res.lastUsage != BufferUsage::None) return;
        }
        if (res.buffer != VK_NULL_HANDLE && cmdBuffer != VK_NULL_HANDLE) {
            VkBufferMemoryBarrier bbar{};
            bbar.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            bbar.srcAccessMask = srcState.accessMask;
            bbar.dstAccessMask = dstState.accessMask;
            bbar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bbar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bbar.buffer = res.buffer;
            bbar.offset = 0;
            bbar.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmdBuffer, srcState.stageMask, dstState.stageMask, 0, 0, nullptr, 1, &bbar, 0, nullptr);
        }
        res.lastUsage = currentUsage;
    };
    for (const auto& [handle, usage] : pass.bufferReads) checkAndAddBufferBarrier(handle, usage);
    for (const auto& [handle, usage] : pass.bufferWrites) checkAndAddBufferBarrier(handle, usage);
}



} // namespace Engine
