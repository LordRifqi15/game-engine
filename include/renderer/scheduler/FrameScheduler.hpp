#pragma once
#include "renderer/scheduler/QueueTypes.hpp"
#include "renderer/scheduler/TimelineSync.hpp"
#include "renderer/graph/RenderGraph.hpp"
#include <vector>
#include <vulkan/vulkan.h>

namespace Engine {

struct CommandBatch {
    QueueType queueType{QueueType::Graphics};
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
    static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;

    FrameScheduler(VkDevice device, const QueueFamilyIndices& indices,
                   VkQueue graphicsQueue = VK_NULL_HANDLE,
                   VkQueue computeQueue = VK_NULL_HANDLE,
                   VkQueue transferQueue = VK_NULL_HANDLE);
    ~FrameScheduler();

    // Non-copyable
    FrameScheduler(const FrameScheduler&) = delete;
    FrameScheduler& operator=(const FrameScheduler&) = delete;

    void resetFrame(uint32_t frameSlot);
    VkCommandBuffer allocateCommandBuffer(uint32_t frameSlot, QueueType queueType);

    void scheduleAndExecute(RenderGraph& graph, uint64_t frameIndex,
                            uint32_t frameSlot = 0,
                            VkSemaphore acquireSemaphore = VK_NULL_HANDLE,
                            VkSemaphore renderFinishedSemaphore = VK_NULL_HANDLE,
                            VkFence completionFence = VK_NULL_HANDLE);

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
    void submitBatches(const std::vector<CommandBatch>& batches,
                       VkFence completionFence);
    void createTimelineSemaphores();
    void destroyTimelineSemaphores();
    void createCommandPools();
    void destroyCommandPools();
    uint32_t familyForQueue(QueueType t) const;

    VkDevice device_{VK_NULL_HANDLE};
    QueueFamilyIndices indices_{};
    QueueContext queues_[static_cast<size_t>(QueueType::Count)];
    uint32_t activeFrameSlot_{0};
};


} // namespace Engine

namespace engine {
    using FrameScheduler = ::Engine::FrameScheduler;
    using CommandBatch = ::Engine::CommandBatch;
}
