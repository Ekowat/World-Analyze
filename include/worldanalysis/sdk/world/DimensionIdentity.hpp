#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace worldanalysis::sdk::dimension_identity {

enum class Kind : std::uint8_t {
    Unknown = 0,
    Overworld,
    Nether,
    TheEnd,
};

inline bool plausible(std::uintptr_t p, std::size_t alignment = alignof(void*)) {
#if UINTPTR_MAX > 0xFFFFFFFFu
    constexpr std::uintptr_t mask = 0x00FFFFFFFFFFFFFFULL;
    constexpr std::uintptr_t max = 0x0010000000000000ULL;
    const auto address = p & mask;
    return address >= 0x10000 && address < max
        && (alignment <= 1 || (address & (alignment - 1)) == 0);
#else
    return p >= 0x10000 && (alignment <= 1 || (p & (alignment - 1)) == 0);
#endif
}

inline bool exactName(const char* raw, std::string_view expected) {
    if (!raw || !plausible(reinterpret_cast<std::uintptr_t>(raw), 1)) return false;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (raw[i] != expected[i]) return false;
    }
    return raw[expected.size()] == '\0';
}

// Android's target uses the Itanium C++ ABI. A polymorphic object's vptr points
// at the vtable address point; vtable[-1] is std::type_info*, and type_info+8
// holds the encoded class name. dot45values.so contains the exact names below.
// Reading RTTI is deliberately preferred over invoking an unverified
// BlockSource virtual merely to obtain a dimension number.
inline Kind kind(const void* dimension) {
    if (!dimension || !plausible(reinterpret_cast<std::uintptr_t>(dimension))) return Kind::Unknown;
    auto* table = *reinterpret_cast<void* const* const*>(dimension);
    if (!table || !plausible(reinterpret_cast<std::uintptr_t>(table))) return Kind::Unknown;
    void* typeInfo = table[-1];
    if (!typeInfo || !plausible(reinterpret_cast<std::uintptr_t>(typeInfo))) return Kind::Unknown;
    const char* name = *reinterpret_cast<const char* const*>(
        static_cast<const std::byte*>(typeInfo) + sizeof(void*));
    if (exactName(name, "18OverworldDimension")) return Kind::Overworld;
    if (exactName(name, "15NetherDimension")) return Kind::Nether;
    if (exactName(name, "15TheEndDimension")) return Kind::TheEnd;
    return Kind::Unknown;
}

// Preserve the conventional persistent cache keys used by existing Tech
// Analysis data: Overworld 0, Nether 1, End 2. Unknown/custom dimensions use
// the reserved -16 bucket rather than invoking an unverified native virtual.
inline int cacheKey(Kind value) {
    switch (value) {
        case Kind::Overworld: return 0;
        case Kind::Nether: return 1;
        case Kind::TheEnd: return 2;
        default: return -16;
    }
}

inline const char* displayName(Kind value) {
    switch (value) {
        case Kind::Overworld: return "OVERWORLD";
        case Kind::Nether: return "NETHER";
        case Kind::TheEnd: return "THE END";
        default: return "UNKNOWN";
    }
}

} // namespace worldanalysis::sdk::dimension_identity
