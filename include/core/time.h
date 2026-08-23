#pragma once

#include <cstdint>

// Frame timing: deltaTime + FPS tracking.
namespace engine {

class Time {
public:
    // Call once at frame start.
    void beginFrame();

    double deltaTime() const { return deltaTime_; }
    double totalTime() const { return totalTime_; }

    // FPS sampled once per second; -1 before first sample.
    double fps() const { return fps_; }

private:
    double nowSeconds() const;

    double lastTime_ = 0.0;
    double deltaTime_ = 0.0;
    double totalTime_ = 0.0;
    double fps_ = -1.0;
    uint32_t framesThisSecond_ = 0;
    double secondAccumulator_ = 0.0;
    bool firstFrame_ = true;
};

} // namespace engine
