#pragma once

namespace engine {

class Window;
class Engine;

// Thin bootstrap: owns window + engine, hands control to engine loop.
class Application {
public:
    Application(int width, int height, const char* title);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void run();

private:
    Window* window_ = nullptr;
    Engine* engine_ = nullptr;
};

} // namespace engine
