#pragma once

#include "../Module.hpp"

namespace worldanalysis::sdk { class Player; }

class MobCounterModule : public Module {
public:
    MobCounterModule();
    ~MobCounterModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    float countRange = 64.0f;
    float notificationDuration = 5.0f;
    float highlightDuration = 1.0f;

    void handleTick(worldanalysis::sdk::Player* player);
};
