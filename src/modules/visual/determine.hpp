#pragma once

#include "../Module.hpp"
#include <cstdint>

namespace worldanalysis::sdk { class Player; }

// Lets other callout modules reserve the opposite side when Determine is
// describing the same block. The query is thread-safe and returns false unless
// Determine currently owns an active block target.
bool activeDetermineBlockCallout(int x, int y, int z, int& side);
// Returns the side of any currently visible Determine callout. Instore uses this
// as a final collision lane guard so its panel can originate from the same o-o
// point/geometry while choosing the opposite side when Determine owns one.
bool activeDetermineCalloutSide(int& side);
// Entity overlays use this to reserve the opposite o-o branch/lane when they
// describe the same actor as Determine.
bool activeDetermineEntityCallout(std::uintptr_t identity, int& side);

class DetermineModule : public Module {
public:
    DetermineModule();
    ~DetermineModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    bool animation = true;
    bool entities = true;
    bool circleChart = false;
    bool blastChart = false;
    bool toolCallout = false;
    float animationSpeed = 1.0f;
    // 0 namespaced identifier, 1 legacy/block item ID, 2 runtime/network ID.
    // User preference: middle mode is the default.
    int blockIdMode = 1;

    void handleTick(worldanalysis::sdk::Player* player);

private:
    bool m_patched = false;
    bool m_modelPatched = false;
    void* m_patchTarget = nullptr;
    void* m_modelPatchTarget = nullptr;
    void applyPatch();
};
