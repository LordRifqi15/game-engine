#pragma once
#include "modules/interaction/Event.hpp"
#include <vector>

namespace Engine {

struct EventInboxComponent {
    std::vector<Event> incomingEvents;

    void post(const Event& evt) {
        incomingEvents.push_back(evt);
    }

    void clear() {
        incomingEvents.clear();
    }
};

} // namespace Engine

namespace engine {
    using EventInboxComponent = ::Engine::EventInboxComponent;
}
