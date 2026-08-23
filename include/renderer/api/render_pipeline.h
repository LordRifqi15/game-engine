#pragma once

#include <cstdint>

// Backend-neutral pipeline interface. No Vulkan types allowed here.
namespace engine {

class RenderPipeline {
public:
    virtual ~RenderPipeline() = default;
};

} // namespace engine
