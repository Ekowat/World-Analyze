#include "itemutils.hpp"

#include <worldanalysis/memory/Signatures.hpp>
#include <worldanalysis/sdk/Offsets.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace worldanalysis::items {
namespace {

using StackGetItemFn = void* (*)(const void*);
using StackGetDamageFn = int (*)(const void*);
using StackGetIdFn = std::uint16_t (*)(const void*);
using I18nGetInstanceFn = void* (*)();
struct EmptySharedPtr { void* object = nullptr; void* control = nullptr; };
using I18nLookupFn = std::string (*)(void*, const std::string&, const EmptySharedPtr&);
// Minecraft 26.45 ItemStackBase / Item layout, re-verified against
// dot45values.so and current generated 26.45 headers.
constexpr std::size_t kStackItemCounter = 0x08;
constexpr std::size_t kStackIdOverride = 0x20;
constexpr std::size_t kStackCount = 0x22;
constexpr std::size_t kSharedCounterItem = 0x00;
// These offsets are recovered from the supplied 26.45 binary. The stack ID
// accessor uses the override at +0x20 unless it is 0x7FFF, then reads the
// definition ID at Item+0x128. Item::getDescriptionId() returns a
// HashedString at +0x90, whose std::string payload starts eight bytes later.
constexpr std::uint16_t kNoStackIdOverride = 0x7FFF;
constexpr std::size_t kItemDefinitionId = 0x128;
constexpr std::size_t kItemDescriptionId = 0x90;
constexpr std::size_t kHashedStringValue = 0x08;
constexpr std::size_t kItemBaseRarity = 0x174;
constexpr std::size_t kI18nLookupSlot = 0x90 / sizeof(void*);
// Current target Item virtual layout. Slot 37 matches the verified
// Item::getMaxDamage entry, which anchors these neighboring method indices.
constexpr std::size_t kItemGetDescriptionIdSlot = 5;
constexpr std::size_t kItemGetRaritySlot = 45;
constexpr std::size_t kItemGetHoverTextColorSlot = 54;
constexpr std::size_t kItemBuildDescriptionNameSlot = 94;

StackGetItemFn g_getItem = nullptr;
StackGetDamageFn g_getDamage = nullptr;
StackGetIdFn g_getId = nullptr;
I18nGetInstanceFn g_getI18n = nullptr;
bool g_initialized = false;

bool plausible(std::uintptr_t p, std::size_t alignment = alignof(void*)) {
#if UINTPTR_MAX > 0xFFFFFFFFu
    // Android 11+ may tag the top byte of every 64-bit heap pointer. ARM TBI
    // preserves that tag on the pointer while ignoring it for memory access.
    // Validate the address portion without destroying the original tag.
    constexpr std::uintptr_t kAddressMask = 0x00FFFFFFFFFFFFFFULL;
    constexpr std::uintptr_t kMaxUserAddress = 0x0010000000000000ULL;
    const std::uintptr_t address = p & kAddressMask;
    if (address < 0x10000 || address >= kMaxUserAddress
        || (alignment > 1 && (address & (alignment - 1)))) return false;
#else
    if (p < 0x10000 || (alignment > 1 && (p & (alignment - 1)))) return false;
#endif
    return true;
}

std::string sanitize(std::string_view value, std::size_t maxLen = 64) {
    std::string out;
    out.reserve(std::min(value.size(), maxLen));
    bool skipFormat = false;
    for (std::size_t i = 0; i < value.size() && out.size() < maxLen; ++i) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (c == 0xC2 && i + 1 < value.size() && static_cast<unsigned char>(value[i + 1]) == 0xA7) {
            ++i;
            skipFormat = true;
            continue;
        }
        if (c == 0xA7) { skipFormat = true; continue; }
        if (skipFormat) { skipFormat = false; continue; }
        if (c >= 32 && c < 127) out.push_back(static_cast<char>(c));
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

bool genericItemName(std::string_view value) {
    std::string lower(value);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lower.empty() || lower == "item" || lower == "item.name" ||
           lower == "minecraft:item" || lower == "unknown item" || lower == "unknown_item";
}

std::string readString(const void* base, std::size_t offset, std::size_t maxLen = 128) {
    if (!base || !plausible(reinterpret_cast<std::uintptr_t>(base))) return {};
    const auto* value = reinterpret_cast<const std::string*>(
        static_cast<const std::byte*>(base) + offset);
    if (value->size() > maxLen) return {};
    return sanitize(*value, maxLen);
}

std::string nativeDescriptionId(void* item) {
    if (!item || !plausible(reinterpret_cast<std::uintptr_t>(item))) return {};
    auto** table = *reinterpret_cast<void***>(item);
    if (!table || !plausible(reinterpret_cast<std::uintptr_t>(table[kItemGetDescriptionIdSlot]), 4)) return {};
    struct HashedStringView {
        std::uint64_t hash;
        std::string value;
        const HashedStringView* lastMatch;
    };
    using Fn = const HashedStringView& (*)(void*);
    const auto& description =
        reinterpret_cast<Fn>(table[kItemGetDescriptionIdSlot])(item);
    if (description.value.size() > 128) return {};
    return sanitize(description.value, 128);
}

std::string nativeDescriptionName(void* item, const void* stack) {
    if (!item || !stack || !plausible(reinterpret_cast<std::uintptr_t>(item))) return {};
    auto** table = *reinterpret_cast<void***>(item);
    if (!table || !plausible(reinterpret_cast<std::uintptr_t>(table[kItemBuildDescriptionNameSlot]), 4)) return {};
    // Target 26.45 RTTI/vtable disassembly shows slot 94 taking Item* in x0,
    // ItemStackBase const* in x1, and returning std::string by value. It calls
    // the same I18n singleton used by Minecraft's own item-name path.
    using Fn = std::string (*)(void*, const void*);
    std::string value = reinterpret_cast<Fn>(table[kItemBuildDescriptionNameSlot])(item, stack);
    if (value.size() > 128) return {};
    value = sanitize(value, 96);
    return genericItemName(value) ? std::string{} : value;
}

int nativeRarity(void* item, const void* stack, int fallback) {
    if (!item || !stack) return fallback;
    auto** table = *reinterpret_cast<void***>(item);
    if (!table || !plausible(reinterpret_cast<std::uintptr_t>(table[kItemGetRaritySlot]), 4)) return fallback;
    using Fn = int (*)(void*, const void*);
    const int value = reinterpret_cast<Fn>(table[kItemGetRaritySlot])(item, stack);
    return value >= 0 && value <= 3 ? value : fallback;
}

std::string nativeHoverColor(void* item, const void* stack) {
    if (!item || !stack) return {};
    auto** table = *reinterpret_cast<void***>(item);
    if (!table || !plausible(reinterpret_cast<std::uintptr_t>(table[kItemGetHoverTextColorSlot]), 4)) return {};
    using Fn = std::string (*)(void*, const void*);
    std::string value = reinterpret_cast<Fn>(table[kItemGetHoverTextColorSlot])(item, stack);
    if (value.size() > 64) return {};
    return value; // preserve section-sign/color tokens until formatColor() parses them
}

void* directStackItem(const void* stack) {
    if (!stack || !plausible(reinterpret_cast<std::uintptr_t>(stack))) return nullptr;
    auto* counter = *reinterpret_cast<void* const*>(
        static_cast<const std::byte*>(stack) + kStackItemCounter);
    if (!counter || !plausible(reinterpret_cast<std::uintptr_t>(counter))) return nullptr;
    void* item = *reinterpret_cast<void**>(
        static_cast<std::byte*>(counter) + kSharedCounterItem);
    return item && plausible(reinterpret_cast<std::uintptr_t>(item)) ? item : nullptr;
}

std::string translate(const std::string& key) {
    if (!g_getI18n || key.empty()) return {};
    void* i18n = g_getI18n();
    if (!i18n || !plausible(reinterpret_cast<std::uintptr_t>(i18n))) return {};
    auto** table = *reinterpret_cast<void***>(i18n);
    if (!table || !plausible(reinterpret_cast<std::uintptr_t>(table[kI18nLookupSlot]), 4)) return {};
    EmptySharedPtr none{};
    std::string value = reinterpret_cast<I18nLookupFn>(table[kI18nLookupSlot])(i18n, key, none);
    if (value.empty() || value == key) return {};
    value = sanitize(value, 64);
    return genericItemName(value) ? std::string{} : value;
}

std::string stripNamespace(std::string value) {
    if (const auto pos = value.find(':'); pos != std::string::npos) value.erase(0, pos + 1);
    return value;
}

bool durabilityEligibleItem(void* item) {
    if (!item || !plausible(reinterpret_cast<std::uintptr_t>(item))) return false;
    // Prefer Minecraft's verified 26.45 Item::getDescriptionId virtual. Some
    // durable utility items do not reliably expose the expected direct string
    // payload through every item-definition layout, which previously caused
    // valid tools such as flint-and-steel to be rejected before damage polling.
    // Fall back to the recovered +0x90 HashedString field only if needed.
    std::string id = nativeDescriptionId(item);
    if (id.empty()) id = readString(item, kItemDescriptionId + kHashedStringValue, 128);
    if (id.empty()) return false;
    std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (id.ends_with(".name")) id.resize(id.size() - 5);
    while (!id.empty() && (id.back() == '_' || id.back() == '.' || id.back() == ':')) id.pop_back();
    auto ends = [&](std::string_view suffix) { return id.ends_with(suffix); };
    std::string compact;
    compact.reserve(id.size());
    for (const unsigned char c : id)
        if (std::isalnum(c)) compact.push_back(static_cast<char>(c));
    auto compactEnds = [&](std::string_view suffix) { return compact.ends_with(suffix); };
    // Deliberately exclude armor, elytra and durability-bearing utility/block items.
    // A held placeable block therefore exits before current-damage/max-damage access.
    // Bedrock has shipped both `flint_and_steel` and compact `flintsteel` description
    // identifiers, so normalize separators before classifying the tool.
    return ends("sword") || ends("pickaxe") || ends("axe") || ends("shovel")
        || ends("hoe") || ends("mace") || ends("trident") || ends("bow")
        || ends("crossbow") || ends("shears") || compactEnds("fishingrod")
        || ends("brush") || compactEnds("flintandsteel") || compactEnds("flintsteel");
}

std::string humanize(std::string value) {
    if (value.empty()) return {};
    value = stripNamespace(value);
    for (const std::string prefix : {std::string("item."), std::string("tile.")}) {
        if (value.rfind(prefix, 0) == 0) value.erase(0, prefix.size());
    }
    if (value.ends_with(".name")) value.resize(value.size() - 5);
    for (char& c : value) if (c == '_' || c == '.') c = ' ';
    bool upper = true;
    for (char& c : value) {
        if (c == ' ') { upper = true; continue; }
        if (upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        upper = false;
    }
    return sanitize(value, 64);
}

std::string displayName26(void* item, const void* stack) {
    if (!item) return {};

    // First use Minecraft's own stack-aware name builder. This is important for
    // variants whose description depends on stack aux/NBT rather than Item alone.
    if (auto value = nativeDescriptionName(item, stack); !value.empty()) return value;

    // Then use the exact 26.45 description ID virtual. Its target body is
    // `return this + 0x90`, but that address is a HashedString rather than a
    // std::string. The text payload is the nested value at +0x98.
    std::string description = nativeDescriptionId(item);
    if (description.empty())
        description = readString(
            item, kItemDescriptionId+kHashedStringValue);
    if (!description.empty() && !genericItemName(description)) {
        if (auto value = translate(description); !value.empty()) return value;
        if (!description.ends_with(".name")) {
            if (auto value = translate(description + ".name"); !value.empty()) return value;
        }
        if (auto nice = humanize(description); !nice.empty() && !genericItemName(nice)) return nice;
    }
    return {};
}

std::uint32_t formatColor(std::string_view raw, int rarity) {
    auto codeColor = [](char c) -> std::uint32_t {
        switch (c) {
        case '0': return 0xFF000000u; case '1': return 0xFF0000AAu; case '2': return 0xFF00AA00u;
        case '3': return 0xFF00AAAAu; case '4': return 0xFFAA0000u; case '5': return 0xFFAA00AAu;
        case '6': return 0xFFFFAA00u; case '7': return 0xFFAAAAAAu; case '8': return 0xFF555555u;
        case '9': return 0xFF5555FFu; case 'a': case 'A': return 0xFF55FF55u;
        case 'b': case 'B': return 0xFF55FFFFu; case 'c': case 'C': return 0xFFFF5555u;
        case 'd': case 'D': return 0xFFFF55FFu; case 'e': case 'E': return 0xFFFFFF55u;
        case 'f': case 'F': return 0xFFFFFFFFu; default: return 0;
        }
    };
    for (std::size_t i = 0; i + 1 < raw.size(); ++i) {
        if (static_cast<unsigned char>(raw[i]) == 0xC2 && i + 2 < raw.size()
            && static_cast<unsigned char>(raw[i + 1]) == 0xA7) {
            if (auto c = codeColor(raw[i + 2])) return c;
        }
        if (static_cast<unsigned char>(raw[i]) == 0xA7) if (auto c = codeColor(raw[i + 1])) return c;
    }
    std::string lower(raw);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (char& c : lower) if (c == ' ' || c == '-') c = '_';
    if (lower.find("dark_blue") != std::string::npos) return 0xFF0000AAu;
    if (lower.find("dark_green") != std::string::npos) return 0xFF00AA00u;
    if (lower.find("dark_aqua") != std::string::npos) return 0xFF00AAAAu;
    if (lower.find("dark_red") != std::string::npos) return 0xFFAA0000u;
    if (lower.find("dark_purple") != std::string::npos) return 0xFFAA00AAu;
    if (lower.find("light_purple") != std::string::npos) return 0xFFFF55FFu;
    if (lower.find("gold") != std::string::npos) return 0xFFFFAA00u;
    if (lower.find("yellow") != std::string::npos) return 0xFFFFFF55u;
    if (lower.find("aqua") != std::string::npos) return 0xFF55FFFFu;
    if (lower.find("green") != std::string::npos) return 0xFF55FF55u;
    if (lower.find("blue") != std::string::npos) return 0xFF5555FFu;
    if (lower.find("red") != std::string::npos) return 0xFFFF5555u;
    return rarityColor(rarity);
}

} // namespace

void initialize() {
    if (g_initialized) return;
    g_initialized = true;
    if (auto a = worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::ItemStackBaseGetItem))
        g_getItem = reinterpret_cast<StackGetItemFn>(a);
    if (auto a = worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::ItemStackBaseGetDamageValue))
        g_getDamage = reinterpret_cast<StackGetDamageFn>(a);
    if (auto a = worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::ItemStackBaseGetId))
        g_getId = reinterpret_cast<StackGetIdFn>(a);
    if (auto a = worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::I18nGetInstance))
        g_getI18n = reinterpret_cast<I18nGetInstanceFn>(a);
}


int maxDamage(const Snapshot& snapshot) {
    if (!snapshot.item || !plausible(reinterpret_cast<std::uintptr_t>(snapshot.item))) return 0;
    auto** table = *reinterpret_cast<void***>(snapshot.item);
    constexpr std::size_t slot = worldanalysis::sdk::offsets::VTable::ItemGetMaxDamage;
    if (!table || !plausible(reinterpret_cast<std::uintptr_t>(table[slot]), 4)) return 0;
    using Fn = int (*)(void*);
    const int value = reinterpret_cast<Fn>(table[slot])(snapshot.item);
    return value > 0 && value < 1000000 ? value : 0;
}

DurabilitySnapshot durability(void* stack) {
    initialize();
    DurabilitySnapshot out{};
    if (!stack || !plausible(reinterpret_cast<std::uintptr_t>(stack))) return out;
    out.count = static_cast<int>(*reinterpret_cast<const std::uint8_t*>(
        static_cast<const std::byte*>(stack) + kStackCount));
    if (out.count <= 0 || out.count > 255) return {};

    // The direct SharedCounter chain is the same verified 26.45 path used by
    // inspect(), but this hot-path probe intentionally stops before any
    // description/rarity/hover virtuals.
    out.item = directStackItem(stack);
    if (!out.item && g_getItem) out.item = g_getItem(stack);
    if (!out.item || !plausible(reinterpret_cast<std::uintptr_t>(out.item))) return {};
    // Critical ordering: identify a true tool/weapon first. Placeable blocks and
    // all other held items never reach the durability getter or Item vtable.
    if (!durabilityEligibleItem(out.item)) return {};
    out.itemKey = reinterpret_cast<std::uintptr_t>(out.item);
    if (!g_getDamage) return {};
    out.damage = g_getDamage(stack);

    auto** table = *reinterpret_cast<void***>(out.item);
    constexpr std::size_t slot = worldanalysis::sdk::offsets::VTable::ItemGetMaxDamage;
    if (!table || !plausible(reinterpret_cast<std::uintptr_t>(table[slot]), 4)) return {};
    using Fn = int (*)(void*);
    const int maximum = reinterpret_cast<Fn>(table[slot])(out.item);
    if (maximum <= 0 || maximum >= 1000000 || out.damage < 0 || out.damage > maximum) return {};
    out.maximum = maximum;
    return out;
}

std::uint32_t rarityColor(int rarity) {
    switch (rarity) {
    case 1: return 0xFFFFFF55u; // uncommon / yellow
    case 2: return 0xFF55FFFFu; // rare / aqua
    case 3: return 0xFFFF55FFu; // epic / light purple
    default: return 0xFFFFFFFFu;
    }
}

Snapshot inspect(void* stack) {
    initialize();
    Snapshot out{};
    out.stack = stack;
    if (!stack || !plausible(reinterpret_cast<std::uintptr_t>(stack))) return out;

    out.count = static_cast<int>(*reinterpret_cast<const std::uint8_t*>(
        static_cast<const std::byte*>(stack) + kStackCount));
    if (out.count <= 0 || out.count > 255) return out;

    // Do both paths and prefer the direct WeakPtr chain. The direct chain is
    // the verified native path and avoids calling a stale wrapper.
    out.item = directStackItem(stack);
    if (!out.item && g_getItem) out.item = g_getItem(stack);
    if (!out.item || !plausible(reinterpret_cast<std::uintptr_t>(out.item))) {
        out.item = nullptr;
        return out;
    }

    if (g_getId) {
        out.id = static_cast<std::int32_t>(g_getId(stack));
    } else {
        // Exact fallback for ItemStackBase::getId() in 26.45.
        const auto stackId = *reinterpret_cast<const std::uint16_t*>(
            static_cast<const std::byte*>(stack)+kStackIdOverride);
        out.id = static_cast<std::int32_t>(
            stackId != kNoStackIdOverride
                ? stackId
                : *reinterpret_cast<const std::uint16_t*>(
                    static_cast<const std::byte*>(out.item)+kItemDefinitionId));
    }
    out.damage = g_getDamage ? g_getDamage(stack) : 0;
    out.itemKey = reinterpret_cast<std::uintptr_t>(out.item);

    const int rawBaseRarity = *reinterpret_cast<const int*>(
        static_cast<const std::byte*>(out.item) + kItemBaseRarity);
    const int baseRarity = std::clamp(rawBaseRarity, 0, 3);
    out.rarity = nativeRarity(out.item, stack, baseRarity);
    out.color = rarityColor(out.rarity);

    // Stack-aware vanilla hover color is authoritative. Preserve the raw
    // formatting string until after parsing section-sign/color tokens.
    const std::string hoverColor = nativeHoverColor(out.item, stack);
    if (!hoverColor.empty()) out.color = formatColor(hoverColor, out.rarity);

    out.name = displayName26(out.item, stack);
    if (out.name.empty()) {
        // This should be exceptional on 26.45, but the true ID keeps different
        // item definitions separate even if localization resources are missing.
        out.name = std::string("ITEM ") + std::to_string(out.id);
    }
    return out;
}


} // namespace worldanalysis::items
