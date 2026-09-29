#pragma once

#include <worldanalysis/sdk/Memory.hpp>
#include <worldanalysis/sdk/Offsets.hpp>

namespace worldanalysis::sdk {

class BlockSource {
public:
    int dimensionId() {
        return virtualCall<int>(this, offsets::VTable::BlockSource_getDimensionId);
    }
};

}
