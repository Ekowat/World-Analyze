#pragma once

#include <worldanalysis/sdk/Memory.hpp>
#include <worldanalysis/sdk/Offsets.hpp>
#include <worldanalysis/sdk/world/HitResult.hpp>

namespace worldanalysis::sdk {

class Level {
public:
    void* actorManager() const { return field<void*>(this, offsets::Level::mActorManager); }

    HitResult* storedHitResult() { return field<HitResult*>(this, offsets::Level::mHitResult); }

    const HitResult* storedHitResult() const { return field<HitResult*>(this, offsets::Level::mHitResult); }
};

}
