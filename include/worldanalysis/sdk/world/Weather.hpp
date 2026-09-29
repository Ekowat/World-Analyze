#pragma once

#include <worldanalysis/sdk/Memory.hpp>
#include <worldanalysis/sdk/Offsets.hpp>

namespace worldanalysis::sdk {

class Weather {
public:
    float oldRainLevel() const { return field<float>(this, offsets::Weather::mOldRainLevel); }
    float rainLevel() const { return field<float>(this, offsets::Weather::mRainLevel); }
    float targetRainLevel() const { return field<float>(this, offsets::Weather::mTargetRainLevel); }
    float oldLightningLevel() const { return field<float>(this, offsets::Weather::mOldLightningLevel); }
    float lightningLevel() const { return field<float>(this, offsets::Weather::mLightningLevel); }
    float targetLightningLevel() const { return field<float>(this, offsets::Weather::mTargetLightningLevel); }

    void setRainLevel(float value) {
        field<float>(this, offsets::Weather::mOldRainLevel) = value;
        field<float>(this, offsets::Weather::mRainLevel) = value;
        field<float>(this, offsets::Weather::mTargetRainLevel) = value;
    }

    void setLightningLevel(float value) {
        field<float>(this, offsets::Weather::mOldLightningLevel) = value;
        field<float>(this, offsets::Weather::mLightningLevel) = value;
        field<float>(this, offsets::Weather::mTargetLightningLevel) = value;
    }
};

}
