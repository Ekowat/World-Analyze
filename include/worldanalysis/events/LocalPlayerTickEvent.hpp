#pragma once

#include <worldanalysis/events/Event.hpp>

namespace worldanalysis::sdk { class Player; }

namespace worldanalysis::events {

struct LocalPlayerTickEvent {
    static constexpr EventType type = EventType::LocalPlayerTick;
    sdk::Player* player;
};

}
