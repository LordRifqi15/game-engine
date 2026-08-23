#pragma once

// Backend-neutral device interface. No Vulkan types allowed here.
namespace engine {

class RenderCommandBuffer;

class RenderDevice {
public:
    virtual ~RenderDevice() = default;

    virtual void waitIdle() = 0;
};

} // namespace engine
