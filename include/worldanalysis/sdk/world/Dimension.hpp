#pragma once

#include <worldanalysis/sdk/Memory.hpp>
#include <worldanalysis/sdk/Offsets.hpp>
#include <worldanalysis/sdk/world/BlockSource.hpp>
#include <worldanalysis/sdk/world/Weather.hpp>

namespace worldanalysis::sdk {

class Dimension {
public:
    BlockSource* blockSource() { return field<BlockSource*>(this, offsets::Dimension::mBlockSource); }
    Weather* weather() { return field<Weather*>(this, offsets::Dimension::mWeather); }
};

}
