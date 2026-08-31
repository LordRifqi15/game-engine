#pragma once
#include <string>

namespace Engine {

struct TagComponent {
    std::string tag{"Entity"};
    TagComponent() = default;
    explicit TagComponent(std::string name) : tag(std::move(name)) {}
};

} // namespace Engine

namespace engine {
    using TagComponent = ::Engine::TagComponent;
}
