#pragma once
#include "RenderGraphResources.hpp"
#include <functional>
#include <vector>

namespace Engine {

class RenderGraph;

class RenderGraphBuilder {
public:
    RenderGraphBuilder(RenderGraph& graph, uint32_t passIndex)
        : graph_(graph), passIndex_(passIndex) {}

    ResourceHandle createTransientImage(const ImageDesc& desc);
    ResourceHandle read(ResourceHandle handle, ResourceUsage usage);
    ResourceHandle write(ResourceHandle handle, ResourceUsage usage);

    BufferHandle createTransientBuffer(const BufferDesc& desc);
    BufferHandle read(BufferHandle handle, BufferUsage usage);
    BufferHandle write(BufferHandle handle, BufferUsage usage);

private:
    RenderGraph& graph_;
    uint32_t passIndex_{0};
};

} // namespace Engine

namespace engine {
    using RenderGraphBuilder = Engine::RenderGraphBuilder;
}
