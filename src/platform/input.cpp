#include "platform/input.h"

#include "platform/window.h"

#include <GLFW/glfw3.h>

namespace engine {

namespace {

GLFWwindow* g_window = nullptr;
double g_lastX = 0.0, g_lastY = 0.0;
float g_deltaX = 0.0f, g_deltaY = 0.0f;
float g_scrollDelta = 0.0f;
bool g_firstMouse = true;

} // namespace
void Input::cursorPosCallback(GLFWwindow* /*window*/, double x, double y) {
    if (g_firstMouse) {
        g_lastX = x;
        g_lastY = y;
        g_firstMouse = false;
    }
    // Accumulate; newFrame() consumes.
    g_deltaX += static_cast<float>(x - g_lastX);
    g_deltaY += static_cast<float>(y - g_lastY);
    g_lastX = x;
    g_lastY = y;
}

void Input::scrollCallback(GLFWwindow* /*window*/, double /*x*/, double y) {
    g_scrollDelta += static_cast<float>(y);
}

void Input::init(Window& window) {
    g_window = window.handle();
    glfwSetCursorPosCallback(g_window, cursorPosCallback);
    glfwSetScrollCallback(g_window, scrollCallback);

    // Lock cursor for mouse-look. ESC handling left to the app layer.
    glfwSetInputMode(g_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
}

void Input::newFrame() {
    // Deltas consumed once per frame; callbacks keep accumulating between polls.
    g_deltaX = 0.0f;
    g_deltaY = 0.0f;
    g_scrollDelta = 0.0f;
    g_firstMouse = false;
}

bool Input::isKeyPressed(int key) {
    return g_window && glfwGetKey(g_window, key) == GLFW_PRESS;
}

bool Input::isMouseButtonPressed(int button) {
    return g_window && glfwGetMouseButton(g_window, button) == GLFW_PRESS;
}

float Input::getMouseDeltaX() { return g_deltaX; }
float Input::getMouseDeltaY() { return g_deltaY; }
float Input::getScrollDelta() { return g_scrollDelta; }

} // namespace engine
