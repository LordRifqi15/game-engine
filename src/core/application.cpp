#include "core/application.h"

#include "core/engine.h"
#include "platform/window.h"

namespace engine {

Application::Application(int width, int height, const char* title) {
    window_ = new Window(width, height, title);
    engine_ = new Engine(*window_);
}

Application::~Application() {
    delete engine_;
    delete window_;
}

void Application::run() {
    engine_->run();
}

} // namespace engine
