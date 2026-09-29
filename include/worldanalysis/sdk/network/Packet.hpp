#pragma once

#include <worldanalysis/sdk/Offsets.hpp>
#include <cstddef>
#include <cstdint>
#include <string>

namespace worldanalysis::sdk {

class Packet {
public:
    void* payload() { return reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(this) + offsets::Packet::Size); }
    const void* payload() const { return reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(this) + offsets::Packet::Size); }
};

class TextPacket : public Packet {
public:
    std::uint32_t variantIndex() const {
        return *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<std::uintptr_t>(payload()) + offsets::TextPacketPayload::mVariantIndex);
    }

    const std::string& message() const {
        const auto base = reinterpret_cast<std::uintptr_t>(payload());
        const auto variant = variantIndex();
        const auto offset = variant == 1
            ? offsets::TextPacketPayload::AuthorAndMessage::mMessage
            : offsets::TextPacketPayload::MessageOnly::mMessage;
        return *reinterpret_cast<const std::string*>(base + offset);
    }
};

}
