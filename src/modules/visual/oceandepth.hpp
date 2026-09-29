#pragma once

#include "../Module.hpp"

namespace worldanalysis::sdk { class Player; }

class OceanDepthModule : public Module {
public:
    OceanDepthModule();
    ~OceanDepthModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Only controls when the panel becomes visible. Once a seed water block is
    // found, the connected-water scan is allowed to continue beyond this radius.
    float notificationRadius = 48.0f;

    void registerLauncherButton();
    void handleTick(worldanalysis::sdk::Player* player);

private:
    bool mButtonRegistered = false;
    bool mScanActive = true;
};
