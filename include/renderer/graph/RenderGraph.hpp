#pragma once
#include "renderer/scheduler/QueueTypes.hpp"
#include "RenderGraphBuilder.hpp"
#include "RenderGraphResources.hpp"
#include "RenderPassNode.hpp"
#include "ResourceLifetime.hpp"
#include <vulkan/vulkan.h>
#include <vector>
#include <memory>
#include <functional>

namespace Engine { class TransientResourcePool; }

namespace Engine {

class RenderGraph {
public:
    RenderGraph() = default;
    ~RenderGraph() = default;

    // 1. Setup Phase - Images
    ResourceHandle importImage(const std::string& name, VkImage image, VkImageView view,
                                VkFormat format, VkExtent2D extent, ResourceUsage initialUsage);

    ResourceHandle createResource(const ImageDesc& desc);

    // 1b. Setup Phase - Buffers
    BufferHandle importBuffer(const std::string& name, VkBuffer buffer, size_t size, BufferUsage initialUsage);
    BufferHandle createBuffer(const BufferDesc& desc);

    template <typename SetupFunc, typename ExecFunc>
    void addPass(const std::string& name, SetupFunc&& setup, ExecFunc&& execute) {
        addPass(name, QueueType::Graphics, std::forward<SetupFunc>(setup), std::forward<ExecFunc>(execute));
    }

    template <typename SetupFunc, typename ExecFunc>
    void addPass(const std::string& name, QueueType queue, SetupFunc&& setup, ExecFunc&& execute) {
        uint32_t passIndex = static_cast<uint32_t>(passes_.size());
        passes_.push_back(RenderPassNode{.name = name, .passIndex = passIndex, .preferredQueue = queue, .actualQueue = queue});
        RenderGraphBuilder builder(*this, passIndex);
        setup(builder);
        passes_[passIndex].executeCallback = std::forward<ExecFunc>(execute);
    }

    // 2. Compilation Phase (DAG sorting + barrier layout derivation)
    bool compile();
    bool compile(uint64_t frameIndex, TransientResourcePool& pool);
    std::vector<ResourceLifetime> computeLifetimes() const;

    // 3. Execution Phase (record barriers and execute pass callbacks)
    void execute(VkCommandBuffer cmdBuffer);
    void execute(VkCommandBuffer cmdBuffer, const std::vector<uint32_t>& passIndices);
    // 4. Reset Phase (wipes transient state for next frame)
    void clear();

    // Accessors for tests / builder
    const std::vector<uint32_t>& sortedPassIndices() const { return sortedPassIndices_; }
    const std::vector<RenderPassNode>& passes() const { return passes_; }
    std::vector<RenderPassNode>& passes() { return passes_; }
    const std::vector<RenderGraphResource>& resources() const { return resources_; }
    const std::vector<RenderGraphBufferResource>& bufferResources() const { return bufferResources_; }
    size_t passCount() const { return passes_.size(); }
    size_t resourceCount() const { return resources_.size(); }
    size_t bufferCount() const { return bufferResources_.size(); }

    // For builder friend access
    void addRead(uint32_t passIndex, ResourceHandle h, ResourceUsage u);
    void addWrite(uint32_t passIndex, ResourceHandle h, ResourceUsage u);
    void addBufferRead(uint32_t passIndex, BufferHandle h, BufferUsage u);
    void addBufferWrite(uint32_t passIndex, BufferHandle h, BufferUsage u);

private:
    void resolveBarriers(uint32_t passIndex, VkCommandBuffer cmdBuffer);

    std::vector<RenderGraphResource> resources_;
    std::vector<RenderGraphBufferResource> bufferResources_;
    std::vector<RenderPassNode> passes_;
    std::vector<uint32_t> sortedPassIndices_;

    friend class RenderGraphBuilder;
};

} // namespace Engine

namespace engine {
    using RenderGraph = ::Engine::RenderGraph;
}
