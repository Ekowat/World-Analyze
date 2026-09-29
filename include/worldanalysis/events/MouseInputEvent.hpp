#pragma once

#include <worldanalysis/events/Event.hpp>

namespace worldanalysis::events {

struct MouseInputEvent : Cancellable {
    static constexpr EventType type = EventType::MouseInput;

    MouseInputEvent(int mouseButton, bool isDown) : button(mouseButton), down(isDown) {}

    int button;
    bool down;
};

}
