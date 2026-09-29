#include <worldanalysis/Api.hpp>
#include <worldanalysis/events/EventBus.hpp>
#include <worldanalysis/memory/Signatures.hpp>
#include "GameHooks.hpp"

namespace {
std::uintptr_t resolveSignature(std::uint16_t id) {
    if (id >= static_cast<std::uint16_t>(worldanalysis::memory::SignatureId::Count)) return 0;
    return worldanalysis::memory::resolve(static_cast<worldanalysis::memory::SignatureId>(id));
}

worldanalysis::sdk::ClientInstance* clientInstance() {
    return reinterpret_cast<worldanalysis::sdk::ClientInstance*>(worldanalysis::core::gamehooks::clientInstance());
}

std::uint64_t subscribe(worldanalysis::events::EventType type, worldanalysis::events::EventPriority priority, worldanalysis::api::EventCallback callback, void* userData) {
    if (!callback) return 0;
    return worldanalysis::events::bus().subscribeRaw(type, [type, callback, userData](void* payload) { callback(type, payload, userData); }, priority);
}

void unsubscribe(std::uint64_t subscription) {
    worldanalysis::events::bus().unsubscribe(subscription);
}

const worldanalysis::api::ApiV1 api{
    worldanalysis::api::AbiVersion,
    sizeof(worldanalysis::api::ApiV1),
    resolveSignature,
    clientInstance,
    subscribe,
    unsubscribe
};
}

extern "C" WORLDANALYSIS_API const worldanalysis::api::ApiV1* WorldAnalysis_GetApi(std::uint32_t version) {
    return version == worldanalysis::api::AbiVersion ? &api : nullptr;
}
