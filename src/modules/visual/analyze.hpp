#pragma once

#include "../Module.hpp"
#include <worldanalysis/sdk/Types.hpp>

namespace worldanalysis::sdk { class Player; }

class AnalyzeModule : public Module {
public:
    AnalyzeModule();
    ~AnalyzeModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    bool onMouseEvent(int button, bool isDown) override;
    bool onTouchInput(int action, int pointerId, float x, float y) override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    float hiddenDuration = 5.0f;
    float resultDuration = 12.0f;
    bool onScreenButton = false;

    void registerLauncherButton();
    void handleTick(worldanalysis::sdk::Player* player);

private:
    bool mButtonRegistered = false;
};

// Shared only for local overlay collision avoidance. Returns the current visible
// Analyze panel center without exposing any target/block/player data.
bool queryAnalyzePanelPosition(worldanalysis::sdk::Vec3& out);
