#pragma once
#include <string>
#include <unordered_set>
#include <cstdint>

namespace Engine {

struct TriggerComponent {
    float radius{1.5f};
    std::string targetTag{"Player"};
    std::string onEnterEvent{"OnEnter"};
    std::string onExitEvent{"OnExit"};
    std::unordered_set<uint32_t> currentOverlaps;
};

} // namespace Engine

namespace engine {
    using TriggerComponent = ::Engine::TriggerComponent;
}
