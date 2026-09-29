#pragma once

#include <worldanalysis/Api.hpp>
#include <worldanalysis/memory/Signatures.hpp>
#include <worldanalysis/sdk/Functions.hpp>
#include <worldanalysis/sdk/Memory.hpp>
#include <worldanalysis/sdk/Offsets.hpp>
#include <worldanalysis/sdk/render/LevelRenderer.hpp>
#include <worldanalysis/sdk/world/Actor.hpp>
#include <worldanalysis/sdk/world/BlockSource.hpp>
#include <dlfcn.h>

namespace worldanalysis::sdk {

class ClientInstance {
public:
    static ClientInstance* current() {
        const auto* runtime = api::find();
        return api::compatible(runtime) && runtime->clientInstance ? runtime->clientInstance() : nullptr;
    }

    BlockSource* region() { return virtualCall<BlockSource*>(this, offsets::VTable::ClientInstance_getRegion); }
    void* camera() {
        using DirectFn = void* (*)(ClientInstance*);
        static DirectFn direct = reinterpret_cast<DirectFn>(dlsym(RTLD_DEFAULT, "_ZN14ClientInstance9getCameraEv"));
        if (direct) return direct(this);
        return virtualCall<void*>(this, offsets::VTable::ClientInstance_getCamera);
    }
    void* minecraftGame() { return virtualCall<void*>(this, offsets::VTable::ClientInstanceGetMinecraftGame); }
    LevelRenderer* levelRenderer() { return field<LevelRenderer*>(this, offsets::ClientInstance::mLevelRenderer); }

    Player* localPlayer(const api::ApiV1* runtime = nullptr) {
        using Function = Player*(*)(ClientInstance*);
        auto target = function<Function>(memory::SignatureId::ClientInstanceGetLocalPlayer, runtime);
        return target ? target(this) : nullptr;
    }

    void* packetSender(const api::ApiV1* runtime = nullptr) {
        using Function = void*(*)(ClientInstance*);
        auto target = function<Function>(memory::SignatureId::ClientInstanceGetPacketSender, runtime);
        return target ? target(this) : nullptr;
    }
};

}
