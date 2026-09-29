#include <worldanalysis/events/EventBus.hpp>

namespace worldanalysis::events {

EventBus& bus() {
    static EventBus instance;
    return instance;
}

}
