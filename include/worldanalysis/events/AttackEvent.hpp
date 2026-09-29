#pragma once

#include <worldanalysis/events/Event.hpp>
#include <cstdint>

namespace worldanalysis::sdk { class Actor; }

namespace worldanalysis::events {

enum class AttackKind : std::uint8_t {
    GameMode,
    SurvivalMode
};

struct AttackEvent : Cancellable {
    static constexpr EventType type = EventType::Attack;

    AttackEvent(AttackKind attackKind, void* mode, sdk::Actor* attackTarget, void* a2, void* a3)
        : kind(attackKind), gameMode(mode), target(attackTarget), argument2(a2), argument3(a3) {}

    AttackKind kind;
    void* gameMode;
    sdk::Actor* target;
    void* argument2;
    void* argument3;
};

}
