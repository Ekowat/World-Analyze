#pragma once

#include "../Module.hpp"

namespace worldanalysis::sdk { class Player; }

class InstoreViewerModule : public Module {
public:
    InstoreViewerModule();
    ~InstoreViewerModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    bool animation = true;
    float animationSpeed = 1.0f;
    bool instoreBoxOutline = true;

    void handleTick(worldanalysis::sdk::Player* player);

};
