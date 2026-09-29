#pragma once

#include "../Module.hpp"
#include <cstdint>

namespace worldanalysis::sdk { class Player; }

class MobPathfindingModule : public Module {
public:
    MobPathfindingModule();
    ~MobPathfindingModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Each native navigation category is enabled and white by default.
    bool hostilePathEnabled = true;
    std::uint32_t hostilePathColor = 0xFFFFFFFFu;
    bool normalPathEnabled = true;
    std::uint32_t normalPathColor = 0xFFFFFFFFu;
    bool otherPathEnabled = true;
    std::uint32_t otherPathColor = 0xFFFFFFFFu;
    bool destinationText = true;
    float pathRange = 48.0f;

    // Retained for ABI/source compatibility with the existing module registry.
    // Native assignment/tick hooks publish paths directly; this only performs
    // stale-cache cleanup.
    void handleTick(worldanalysis::sdk::Player* player);
};
