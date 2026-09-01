#pragma once
#include "renderer/scheduler/QueueTypes.hpp"
#include "renderer/scheduler/TimelineSync.hpp"
#include "renderer/graph/RenderGraph.hpp"
#include <vector>
#include <vulkan/vulkan.h>

namespace Engine {

struct CommandBatch {
    QueueType queueType;
    VkCommandBuffer cmdBuffer{VK_NULL_HANDLE};
    std::vector<uint32_t> passIndices;
    
    // Dependencies to wait on before executing this batch
    std::vector<VkSemaphore> waitSemaphores;
    std::vector<uint64_t> waitValues;
    std::vector<VkPipelineStageFlags> waitStageMasks;

    // Signals dispatched upon completion of this batch
    std::vector<VkSemaphore> signalSemaphores;
    std::vector<uint64_t> signalValues;
};

class FrameScheduler {
public:
    FrameScheduler(VkDevice device, const QueueFamilyIndices& indices);
    ~FrameScheduler();

    // Non-copyable
    FrameScheduler(const FrameScheduler&) = delete;
    FrameScheduler& operator=(const FrameScheduler&) = delete;

    void scheduleAndExecute(RenderGraph& graph, uint64_t frameIndex);

    // Exposed for tests: partition sorted DAG into batches
    void partitionDAG(RenderGraph& graph, std::vector<CommandBatch>& outBatches);

    // For tests: inspect queue contexts
    const QueueContext& queueContext(QueueType t) const { return queues_[static_cast<size_t>(t)]; }
    QueueContext& queueContext(QueueType t) { return queues_[static_cast<size_t>(t)]; }

    // Ownership transfer helper (also exposed for validation)
    void injectQueueOwnershipTransfer(
        VkCommandBuffer srcCmdBuffer,
        VkCommandBuffer dstCmdBuffer,
        const RenderGraphResource& resource,
        uint32_t srcFamily,
        uint32_t dstFamily,
        ResourceUsage srcUsage,
        ResourceUsage dstUsage);

    // Buffer variant
    void injectQueueOwnershipTransfer(
        VkCommandBuffer srcCmdBuffer,
        VkCommandBuffer dstCmdBuffer,
        const RenderGraphBufferResource& resource,
        uint32_t srcFamily,
        uint32_t dstFamily,
        BufferUsage srcUsage,
        BufferUsage dstUsage);

private:
    void submitBatches(const std::vector<CommandBatch>& batches);
    void createTimelineSemaphores();
    void destroyTimelineSemaphores();
    uint32_t familyForQueue(QueueType t) const;

    VkDevice device_{VK_NULL_HANDLE};
    QueueFamilyIndices indices_{};
    QueueContext queues_[static_cast<size_t>(QueueType::Count)];
};

} // namespace Engine

namespace engine {
    using FrameScheduler = Engine::FrameScheduler;
    using CommandBatch = Engine::CommandBatch;
}
