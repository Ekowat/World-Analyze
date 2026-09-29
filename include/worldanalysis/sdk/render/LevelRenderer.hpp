#pragma once

#include <worldanalysis/sdk/Memory.hpp>
#include <worldanalysis/sdk/Offsets.hpp>
#include <worldanalysis/sdk/render/LevelRendererPlayer.hpp>

namespace worldanalysis::sdk {

class LevelRenderer {
public:
    void* renderChunkCoordinatorTable() {
        return reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(this) + offsets::LevelRenderer::mRenderChunkCoordinators);
    }

    LevelRendererPlayer* playerRenderer() { return field<LevelRendererPlayer*>(this, offsets::LevelRenderer::mLevelRendererPlayer); }
};

}
