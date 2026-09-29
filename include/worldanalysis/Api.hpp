#pragma once

#include <worldanalysis/Export.hpp>
#include <worldanalysis/events/Events.hpp>
#include <worldanalysis/memory/Signatures.hpp>
#include <cstddef>
#include <cstdint>
#include <dlfcn.h>

namespace worldanalysis::sdk { class ClientInstance; }

namespace worldanalysis::api {

inline constexpr std::uint32_t AbiVersion = 1;
using EventCallback = void(*)(events::EventType type, void* payload, void* userData);

struct ApiV1 {
    std::uint32_t abiVersion;
    std::uint32_t structSize;
    std::uintptr_t (*resolveSignature)(std::uint16_t id);
    sdk::ClientInstance* (*clientInstance)();
    std::uint64_t (*subscribe)(events::EventType type, events::EventPriority priority, EventCallback callback, void* userData);
    void (*unsubscribe)(std::uint64_t subscription);
};

inline bool compatible(const ApiV1* runtime) {
    return runtime && runtime->abiVersion == AbiVersion && runtime->structSize >= sizeof(ApiV1);
}

inline std::uintptr_t resolve(memory::SignatureId id, const ApiV1* runtime) {
    return compatible(runtime) && runtime->resolveSignature ? runtime->resolveSignature(static_cast<std::uint16_t>(id)) : 0;
}

using GetApiFunction = const ApiV1*(*)(std::uint32_t version);

inline const ApiV1* find(std::uint32_t version = AbiVersion) {
    void* handle = dlopen("libWorldAnalysis.so", RTLD_NOW | RTLD_NOLOAD);
    if (!handle) return nullptr;
    auto getter = reinterpret_cast<GetApiFunction>(dlsym(handle, "WorldAnalysis_GetApi"));
    const ApiV1* result = getter ? getter(version) : nullptr;
    dlclose(handle);
    return compatible(result) ? result : nullptr;
}

}

extern "C" WORLDANALYSIS_API const worldanalysis::api::ApiV1* WorldAnalysis_GetApi(std::uint32_t version);
