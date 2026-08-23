#pragma once

#include <cstdint>

// Backend-neutral swapchain interface. No Vulkan types allowed here.
namespace engine {

class RenderSwapchain {
public:
    virtual ~RenderSwapchain() = default;

    // Returns true when the swapchain is usable for the acquired image.
    // False means: caller should recreate and skip this frame.
    virtual bool acquireNextImage(uint32_t frameIndex) = 0;

    // Submits recorded work for imageIndex and presents.
    virtual void present(uint32_t imageIndex, uint32_t frameIndex) = 0;

    virtual uint32_t imageCount() const = 0;
    virtual uint32_t width() const = 0;
    virtual uint32_t height() const = 0;
};

} // namespace engine
