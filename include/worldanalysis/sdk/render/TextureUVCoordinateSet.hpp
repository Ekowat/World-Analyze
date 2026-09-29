#pragma once

#include <worldanalysis/sdk/Memory.hpp>
#include <worldanalysis/sdk/Offsets.hpp>

namespace worldanalysis::sdk {

class TextureUVCoordinateSet {
public:
    float& u0() { return field<float>(this, offsets::TextureUVCoordinateSet::mU0); }
    float& v0() { return field<float>(this, offsets::TextureUVCoordinateSet::mV0); }
    float& u1() { return field<float>(this, offsets::TextureUVCoordinateSet::mU1); }
    float& v1() { return field<float>(this, offsets::TextureUVCoordinateSet::mV1); }
    const float& u0() const { return field<float>(this, offsets::TextureUVCoordinateSet::mU0); }
    const float& v0() const { return field<float>(this, offsets::TextureUVCoordinateSet::mV0); }
    const float& u1() const { return field<float>(this, offsets::TextureUVCoordinateSet::mU1); }
    const float& v1() const { return field<float>(this, offsets::TextureUVCoordinateSet::mV1); }
};

}
