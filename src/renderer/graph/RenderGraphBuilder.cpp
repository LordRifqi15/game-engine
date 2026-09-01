#include "renderer/graph/RenderGraphBuilder.hpp"
#include "renderer/graph/RenderGraph.hpp"

namespace Engine {

ResourceHandle RenderGraphBuilder::createTransientImage(const ImageDesc& desc) {
    ResourceHandle h = graph_.createResource(desc);
    // creation implies write for this pass (caller may add explicit write with usage)
    // ponytail: don't auto-register write usage; caller decides via write()
    return h;
}

ResourceHandle RenderGraphBuilder::read(ResourceHandle handle, ResourceUsage usage) {
    graph_.addRead(passIndex_, handle, usage);
    return handle;
}

ResourceHandle RenderGraphBuilder::write(ResourceHandle handle, ResourceUsage usage) {
    graph_.addWrite(passIndex_, handle, usage);
    return handle;
}

} // namespace Engine
