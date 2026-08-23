#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <cstdint>

namespace engine {

class Window {
public:
    Window(int width, int height, const char* title);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool shouldClose() const;
    void pollEvents();
    void waitEvents();

    // Returns false if the window was closed while waiting.
    void waitUntilClosed();

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    GLFWwindow* handle() const { return window_; }

private:
    static void framebufferResizeCallback(GLFWwindow* window, int width, int height);

    GLFWwindow* window_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

} // namespace engine
