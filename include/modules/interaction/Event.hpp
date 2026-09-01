#pragma once
#include <string>
#include <cstdint>

namespace Engine {

struct Event {
    std::string name;
    uint32_t instigatorEntity{0};
    uint32_t targetEntity{0};
    float value{0.0f};
};

} // namespace Engine

namespace engine {
    using Event = ::Engine::Event;
}
