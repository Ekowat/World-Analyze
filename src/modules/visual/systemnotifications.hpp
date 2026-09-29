#pragma once

#include "../Module.hpp"
#include <string_view>

namespace worldanalysis::sdk { class Player; }

class SystemNotificationsModule : public Module {
public:
    SystemNotificationsModule();
    ~SystemNotificationsModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    bool weatherNotifications = true;
    bool timeNotifications = true;
    bool levelNotifications = true;
    bool slimeChunkNotifications = true;
    bool sleepDeprivedNotifications = true;
    bool hpLowNotifications = true;
    bool dimensionNotifications = true;
    bool dayRolloverNotifications = false;
    bool hungerLowNotifications = true;
    bool saturationEmptyNotifications = false;
    bool levelDownNotifications = true;
    bool healthRecoveredNotifications = true;
    bool moduleNotifications = true;

    // A slime chunk can briefly expose its outline. 0 is terrain-following
    // Ground mode; 100 smoothly grows the same perimeter through the full
    // Overworld height. Intermediate slider values blend the two modes.
    bool slimeChunkGrid = false;
    int slimeChunkGridMode = 0;
    float slimeChunkGridDuration = 8.0f;

    float notificationDuration = 4.5f;
    float animationSpeed = 1.0f;

    void handleTick(worldanalysis::sdk::Player* player);
    void notifyModuleState(std::string_view moduleName, bool poweredOn);
};
