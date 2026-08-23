#include "core/application.h"

int main() {
    engine::Application app(1280, 720, "Game Engine");
    app.run();
    return 0;
}
