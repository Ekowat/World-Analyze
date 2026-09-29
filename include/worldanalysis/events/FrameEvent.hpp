#pragma once

#include <worldanalysis/events/Event.hpp>

namespace worldanalysis::events {

struct FrameEvent {
    static constexpr EventType type = EventType::Frame;
};

}
