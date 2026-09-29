#pragma once

#include <worldanalysis/events/Event.hpp>
#include <cstdint>

namespace worldanalysis::events {

enum class ScreenKind : std::uint8_t {
    Container,
    Chat
};

enum class ScreenPhase : std::uint8_t {
    Opened,
    Closed
};

struct ScreenStateEvent {
    static constexpr EventType type = EventType::ScreenState;
    ScreenKind screen;
    ScreenPhase phase;
    void* controller;
};

}
