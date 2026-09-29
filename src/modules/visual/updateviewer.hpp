#pragma once

#include "../Module.hpp"

namespace worldanalysis::sdk { class Player; }

class UpdateViewerModule : public Module {
public:
    UpdateViewerModule();
    ~UpdateViewerModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    bool blockUpdates = true;
    bool redstoneUpdates = true;
    bool cropUpdates = true;
    bool fluidUpdates = true;
    bool mobSpawns = true;
    bool ambientTechAnimations = false;
    float duration = 4.0f;
    float eventRadius = 18.0f;

    void handleTick(worldanalysis::sdk::Player* player);
};
