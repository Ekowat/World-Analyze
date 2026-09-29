#pragma once

#include <worldanalysis/events/Event.hpp>

namespace worldanalysis::sdk { class Player; }

namespace worldanalysis::events {

// Fired immediately before Minecraft processes the local player's normal tick.
// Modules can use this to consume/adjust input before vanilla movement logic runs.
struct LocalPlayerPreTickEvent {
    static constexpr EventType type = EventType::LocalPlayerPreTick;
    sdk::Player* player;
};

}
