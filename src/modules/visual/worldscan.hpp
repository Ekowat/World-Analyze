#pragma once

#include "../Module.hpp"

namespace worldanalysis::sdk { class Player; }

class WorldScanModule : public Module {
public:
    WorldScanModule();
    ~WorldScanModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    float buttonScale = 1.0f;
    float buttonOpacity = 0.82f;
    int scanChunks = 5;          // radius in chunks represented by the miniature
    int surfaceResolution = 40;  // legacy heightmap resolution
    bool legacyMode = true;      // old sampled plane-heightmap renderer (default for performance)

    void registerLauncherButton();
    void handleTick(worldanalysis::sdk::Player* player);

private:
    bool mButtonRegistered = false;
};
