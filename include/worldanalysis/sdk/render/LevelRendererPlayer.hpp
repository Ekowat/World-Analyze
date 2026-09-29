#pragma once

#include <worldanalysis/sdk/Memory.hpp>
#include <worldanalysis/sdk/Offsets.hpp>
#include <worldanalysis/sdk/Types.hpp>

namespace worldanalysis::sdk {

class LevelRendererPlayer {
public:
    float& fogColorRed() { return field<float>(this, offsets::LevelRendererPlayer::mFogColorRed); }
    float& fogColorGreen() { return field<float>(this, offsets::LevelRendererPlayer::mFogColorGreen); }
    float& fogColorBlue() { return field<float>(this, offsets::LevelRendererPlayer::mFogColorBlue); }
    float& baseFogStart() { return field<float>(this, offsets::LevelRendererPlayer::mBaseFogStart); }
    float& baseFogEnd() { return field<float>(this, offsets::LevelRendererPlayer::mBaseFogEnd); }
    float& currentFogDensityMax() { return field<float>(this, offsets::LevelRendererPlayer::mCurrentFogDensityMax); }
    Vec3& cameraPosition() { return field<Vec3>(this, offsets::LevelRendererPlayer::mCamPos); }
    void*& selectionOverlayMaterial() { return field<void*>(this, offsets::LevelRendererPlayer::mSelectionOverlayMaterial); }
};

}
