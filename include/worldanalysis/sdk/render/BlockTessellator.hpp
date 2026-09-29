#pragma once

#include <worldanalysis/sdk/Memory.hpp>
#include <worldanalysis/sdk/Offsets.hpp>
#include <worldanalysis/sdk/world/BlockSource.hpp>

namespace worldanalysis::sdk {

class BlockTessellator {
public:
    BlockSource* region() { return field<BlockSource*>(this, offsets::BlockTessellator::mRegion); }
    bool usesInternalTexture() const { return field<std::uint8_t>(this, offsets::BlockTessellator::mUseInternalTexture) != 0; }
    std::uint8_t& xFlipTexture() { return field<std::uint8_t>(this, offsets::BlockTessellator::mXFlipTexture); }
    void* internalTexture() { return reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(this) + offsets::BlockTessellator::mInternalTexture); }
};

}
