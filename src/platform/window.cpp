#include "platform/window.h"

#include <cstdio>
#include <cstdlib>

namespace engine {

Window::Window(int width, int height, const char* title)
    : width_(width), height_(height) {
    if (!glfwInit()) {
        std::fputs("Fatal: failed to initialize GLFW\n", stderr);
        std::exit(EXIT_FAILURE);
    }

    // Vulkan only: no OpenGL context.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    // Resizing handled explicitly once swapchain exists.
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    window_ = glfwCreateWindow(width_, height_, title, nullptr, nullptr);
    if (!window_) {
        std::fputs("Fatal: failed to create GLFW window\n", stderr);
        glfwTerminate();
        std::exit(EXIT_FAILURE);
    }

    glfwSetWindowUserPointer(window_, this);
    glfwSetFramebufferSizeCallback(window_, framebufferResizeCallback);
}

Window::~Window() {
    if (window_) {
        glfwDestroyWindow(window_);
    }
    glfwTerminate();
}

bool Window::shouldClose() const {
    return glfwWindowShouldClose(window_);
}

void Window::pollEvents() {
    glfwPollEvents();
}

void Window::waitEvents() {
    glfwWaitEvents();
}

void Window::waitUntilClosed() {
    while (!shouldClose()) {
        waitEvents();
    }
}

void Window::framebufferResizeCallback(GLFWwindow* window, int width, int height) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    self->width_ = width;
    self->height_ = height;
}

} // namespace engine
