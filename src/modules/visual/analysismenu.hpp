#pragma once

#include "../Module.hpp"

namespace worldanalysis::sdk { class Player; }

class AnalysisMenuModule : public Module {
public:
    AnalysisMenuModule();
    ~AnalysisMenuModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    bool onMouseEvent(int button, bool isDown) override;
    bool onTouchInput(int action, int pointerId, float x, float y) override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    float hiddenDuration = 5.0f;

    void handleTick(worldanalysis::sdk::Player* player);
};
