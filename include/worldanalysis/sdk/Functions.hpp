#pragma once

#include <worldanalysis/Api.hpp>
#include <worldanalysis/memory/Signatures.hpp>

namespace worldanalysis::sdk {

template <class Function>
Function function(memory::SignatureId id, const api::ApiV1* runtime = nullptr) {
    if (!runtime) runtime = api::find();
    const auto address = api::resolve(id, runtime);
    return address ? reinterpret_cast<Function>(address) : nullptr;
}

}
