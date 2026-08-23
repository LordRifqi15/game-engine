#pragma once

#include <GLFW/glfw3.h>

namespace engine {

class Window;

class Input {
public:
    static void init(Window& window);

    // Call once per frame BEFORE polling consumers (after glfwPollEvents).
    static void newFrame();

    static bool isKeyPressed(int key);
    static bool isMouseButtonPressed(int button);

    // Mouse position delta since last frame. Zero on first frame.
    static float getMouseDeltaX();
    static float getMouseDeltaY();

    // Scroll delta accumulated since last frame.
    static float getScrollDelta();

private:
    static void cursorPosCallback(GLFWwindow* window, double x, double y);
    static void scrollCallback(GLFWwindow* window, double x, double y);
};

} // namespace engine
