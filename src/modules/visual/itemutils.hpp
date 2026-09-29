#pragma once

#include <cstdint>
#include <string>

namespace worldanalysis::items {

struct Snapshot {
    void* stack = nullptr;
    void* item = nullptr;
    std::uintptr_t itemKey = 0;
    std::int32_t id = 0;
    std::int32_t damage = 0;
    int count = 0;
    int rarity = 0;
    std::uint32_t color = 0xFFFFFFFFu;
    std::string name;

    bool valid() const { return stack && item && count > 0; }
};

struct DurabilitySnapshot {
    void* item = nullptr;
    std::uintptr_t itemKey = 0;
    int damage = 0;
    int maximum = 0;
    int count = 0;

    bool valid() const { return item && itemKey && count > 0 && maximum > 0; }
};

void initialize();
Snapshot inspect(void* stack);
// Returns the native maximum durability for durable items, or 0 for
// non-damageable/invalid items. Slot 37 is verified for the 26.45 Item vtable.
int maxDamage(const Snapshot& snapshot);
// Minimal held-item durability probe. Unlike inspect(), this deliberately does
// not call item naming/rarity/hover virtuals, making it safe for hot polling
// while the player is actively placing or consuming items.
DurabilitySnapshot durability(void* stack);
std::uint32_t rarityColor(int rarity);

} // namespace worldanalysis::items
