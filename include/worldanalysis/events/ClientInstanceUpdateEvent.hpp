#pragma once

#include <worldanalysis/events/Event.hpp>

namespace worldanalysis::sdk { class ClientInstance; }

namespace worldanalysis::events {

struct ClientInstanceUpdateEvent {
    static constexpr EventType type = EventType::ClientInstanceUpdate;
    sdk::ClientInstance* clientInstance;
};

}
