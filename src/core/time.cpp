#include "core/time.h"

#include <GLFW/glfw3.h>

namespace engine {

double Time::nowSeconds() const {
    return glfwGetTime();
}

void Time::beginFrame() {
    double now = nowSeconds();
    if (firstFrame_) {
        firstFrame_ = false;
        lastTime_ = now;
        deltaTime_ = 0.0;
        return;
    }

    deltaTime_ = now - lastTime_;
    lastTime_ = now;
    totalTime_ += deltaTime_;

    // FPS sampling window (1s).
    ++framesThisSecond_;
    secondAccumulator_ += deltaTime_;
    if (secondAccumulator_ >= 1.0) {
        fps_ = static_cast<double>(framesThisSecond_) / secondAccumulator_;
        framesThisSecond_ = 0;
        secondAccumulator_ = 0.0;
    }
}

} // namespace engine
