#include "ModuleMenu.hpp"
#include "modules/ModuleRegistry.hpp"
#include "config/ConfigManager.hpp"
#include "modules/visual/systemnotifications.hpp"
#include "modules/visual/worldscan.hpp"
#include "modules/visual/analyze.hpp"
#include "modules/visual/oceandepth.hpp"
#include <pl/ModMenu.hpp>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>


static void notifyModuleStateChange(Module* mod, bool previousEnabled) {
    if (!mod || previousEnabled == mod->enabled || mod->moduleId == "worldanalysis.System Notifications") return;
    auto* base = ModuleRegistry::get().find("worldanalysis.System Notifications");
    if (!base) return;
    static_cast<SystemNotificationsModule*>(base)->notifyModuleState(mod->name, mod->enabled);
}

static void onModuleToggle(std::string_view module_id, bool enabled) {
    auto* mod = ModuleRegistry::get().find(module_id);
    if (!mod) return;
    const bool previousEnabled = mod->enabled;
    mod->setMasterEnabled(enabled);
    notifyModuleStateChange(mod, previousEnabled);
    worldanalysis::config::ConfigManager::get().save();
}

static void onModuleKeybind(std::string_view module_id, std::string_view key, bool isDown) {
    if (ModuleRegistry::get().keybindBlocked()) return;
    auto* mod = ModuleRegistry::get().find(module_id);
    if (!mod) return;
    const bool previousEnabled = mod->enabled;
    mod->onKeybindEvent(std::string(key), isDown);
    if (isDown) notifyModuleStateChange(mod, previousEnabled);
}

static void onModuleConfigChanged(std::string_view module_id, std::string_view key, std::string_view value) {
    auto* mod = ModuleRegistry::get().find(module_id);
    if (!mod) return;

    nlohmann::json j;
    mod->saveConfig(j);

    std::string safeValue(value);
    std::string safeKey(key);
    if (!safeValue.empty()) {
        try {
            if (j.contains(safeKey)) {
                if (j[safeKey].is_boolean()) {
                    if (safeValue == "true") j[safeKey] = true;
                    else if (safeValue == "false") j[safeKey] = false;
                } else if (j[safeKey].is_number_integer()) {
                    char* end;
                    int val = std::strtol(safeValue.c_str(), &end, 10);
                    if (end != safeValue.c_str()) j[safeKey] = val;
                } else if (j[safeKey].is_number_float()) {
                    char* end;
                    float val = std::strtof(safeValue.c_str(), &end);
                    if (end != safeValue.c_str()) j[safeKey] = val;
                } else {
                    j[safeKey] = safeValue;
                }
            } else {
                j[safeKey] = safeValue;
            }
        } catch (...) {
            j[safeKey] = safeValue;
        }
    }
    mod->loadConfig(j);
    if (mod->moduleId == "worldanalysis.World Scan") {
        std::string lowerKey(safeKey);
        std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowerKey == "color" || lowerKey == "backgroundcolor" || lowerKey == "backgroundopacity" ||
            lowerKey == "buttonscale" || lowerKey == "buttonopacity") {
            static_cast<WorldScanModule*>(mod)->registerLauncherButton();
        }
    }
    if (mod->moduleId == "worldanalysis.Analyze") {
        std::string lowerKey(safeKey);
        std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowerKey == "onscreenbutton" || lowerKey == "color" || lowerKey == "backgroundcolor" || lowerKey == "backgroundopacity")
            static_cast<AnalyzeModule*>(mod)->registerLauncherButton();
    }
    if (mod->moduleId == "worldanalysis.Ocean Depth") {
        std::string lowerKey(safeKey);
        std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowerKey == "color" || lowerKey == "backgroundcolor" || lowerKey == "backgroundopacity")
            static_cast<OceanDepthModule*>(mod)->registerLauncherButton();
    }
    worldanalysis::config::ConfigManager::get().save();
}

void registerModulesWithLauncher(std::string_view ownerId) {
    auto& modules = ModuleRegistry::get().modules();

    for (auto* mod : modules) {
        if (!mod->showInMenu) continue;

        pl::modmenu::ModuleBuilder builder(mod->moduleId, mod->name);
        builder.modId(std::string(ownerId))
                .description(mod->description)
                .defaultEnabled(mod->masterEnabled)
                .hideInHudEditor(mod->hideInHudEditor)
                .onToggle(onModuleToggle)
                .onConfigChanged(onModuleConfigChanged)
                .onKeybind(onModuleKeybind);

        nlohmann::json j;
        mod->saveConfig(j);

        static const char* baseKeys[] = {
                "enabled", "masterEnabled", "keybindActive", "shortcutEnabled", "shortcutSize", "shortcutOpacity",
                "lockPosition", "shortcutPosX", "shortcutPosY",
                "isHudModule", "hudPosX", "hudPosY"
        };

        struct TmpConfigEntry {
            std::string key;
            std::string displayName;
            pl::modmenu::ConfigType type;
            std::string default_value;
            std::string min_value;
            std::string max_value;
            std::string depends_on;
        };
        std::vector<TmpConfigEntry> configs;

        for (auto& [k, v] : j.items()) {
            bool isBase = false;
            for (const char* bk : baseKeys) {
                if (k == bk) { isBase = true; break; }
            }
            if (isBase) continue;
            // Mob Pathfinding has independent hostile/normal/other color pickers.
            // Do not expose the generic Module color as a fourth misleading color.
            if (mod->moduleId == "worldanalysis.Mob Pathfinding" && k == "color") continue;

            std::string displayName;
            std::string sourceKey = k;
            if (sourceKey.size() > 2 && sourceKey[0] == 'm' && sourceKey[1] == '_') {
                sourceKey = sourceKey.substr(2);
            }
            for (size_t i = 0; i < sourceKey.size(); ++i) {
                if (i == 0) {
                    displayName += toupper(sourceKey[i]);
                } else if (isupper(sourceKey[i])) {
                    displayName += ' ';
                    displayName += sourceKey[i];
                } else {
                    displayName += sourceKey[i];
                }
            }

            TmpConfigEntry entry;
            entry.key = k;
            entry.displayName = displayName;
            if (k == "moduleNotifications")
                entry.displayName = "Module Notification";
            else if (k == "slimeChunkGridMode")
                entry.displayName = "Slime Grid Mode: Ground - Full Height";
            else if (k == "slimeChunkGridDuration")
                entry.displayName = "Slime Grid Duration (Seconds)";
            else if (k == "legacyMode")
                entry.displayName = "Legacy Mode (Heightmap)";
            else if (k == "surfaceResolution" && mod->moduleId == "worldanalysis.World Scan")
                entry.displayName = "Legacy Surface Resolution";
            else if (k == "highlightDuration" && mod->moduleId == "worldanalysis.Mob Counter")
                entry.displayName = "Highlight Duration (Seconds)";
            else if (k == "notificationDuration" && mod->moduleId == "worldanalysis.Mob Counter")
                entry.displayName = "Notification Duration (Seconds)";

            if (v.is_boolean()) {
                std::string kLower = k;
                std::transform(kLower.begin(), kLower.end(), kLower.begin(), ::tolower);
                if (mod->moduleId == "worldanalysis.Analyze" && kLower == "onscreenbutton") {
                    entry.type = pl::modmenu::ConfigType::Toggle;
                } else if (kLower.find("button") != std::string::npos) {
                    entry.type = pl::modmenu::ConfigType::Button;
                } else {
                    entry.type = pl::modmenu::ConfigType::Toggle;
                }
                entry.default_value = v.get<bool>() ? "true" : "false";
            } else if (v.is_number_integer()) {
                entry.type = pl::modmenu::ConfigType::SliderInt;
                entry.default_value = std::to_string(v.get<int>());

                std::string kLower = k;
                std::transform(kLower.begin(), kLower.end(), kLower.begin(), ::tolower);

                if (kLower.find("keybind") != std::string::npos) {
                    entry.type = pl::modmenu::ConfigType::Keybind;
                } else {
                    int minVal = 0;
                    int maxVal = 200;
                    if (kLower == "maxactive") {
                        minVal = 24;
                        maxVal = 96;
                    } else if (kLower == "blockidmode") {
                        minVal = 0;
                        maxVal = 2;
                    } else if (kLower == "slimechunkgridmode") {
                        // 0 = terrain-following Ground mode, 100 = Full Height.
                        // Intermediate positions animate/blend between them.
                        minVal = 0;
                        maxVal = 100;
                    } else if (kLower == "outlinechunks") {
                        minVal = 2;
                        maxVal = 12;
                    } else if (kLower == "scanchunks") {
                        minVal = 2;
                        maxVal = 12;
                    } else if (kLower == "surfaceresolution") {
                        minVal = 20;
                        maxVal = 64;
                    } else if (kLower == "verticalrange") {
                        minVal = 8;
                        maxVal = 48;
                    } else if (kLower == "horizontaldistance") {
                        minVal = 4;
                        maxVal = 96;
                    } else if (kLower == "verticalheight") {
                        minVal = 4;
                        maxVal = 128;
                    } else if (kLower.find("cps") != std::string::npos) {
                        minVal = 1;
                        maxVal = 30;
                    } else if (kLower.find("points") != std::string::npos ||
                               kLower.find("steps") != std::string::npos) {
                        minVal = 1;
                        maxVal = 2000;
                    } else if (kLower.find("time") != std::string::npos) {
                        maxVal = 24000;
                    }

                    entry.min_value = std::to_string(minVal);
                    entry.max_value = std::to_string(maxVal);
                }
            } else if (v.is_number_float()) {
                entry.type = pl::modmenu::ConfigType::SliderFloat;
                entry.default_value = std::to_string(v.get<float>());

                std::string kLower = k;
                std::transform(kLower.begin(), kLower.end(), kLower.begin(), ::tolower);

                float minVal = 0.0f;
                float maxVal = 100.0f;

                if (kLower.size() >= 9 && kLower.ends_with("frequency")) {
                    minVal = 0.05f;
                    maxVal = 3.00f;
                } else if (kLower == "spawnrate") {
                    minVal = 1.00f;
                    maxVal = 18.00f;
                } else if (kLower == "effectrange") {
                    minVal = 96.0f;
                    maxVal = 384.0f;
                } else if (kLower == "sizescale") {
                    minVal = 0.50f;
                    maxVal = 2.50f;
                } else if (kLower == "nearrange") {
                    minVal = 8.0f;
                    maxVal = 30.0f;
                } else if (kLower == "midrange") {
                    minVal = 24.0f;
                    maxVal = 96.0f;
                } else if (kLower == "farrange") {
                    minVal = 48.0f;
                    maxVal = 160.0f;
                } else if (kLower == "largemindistance") {
                    minVal = 18.0f;
                    maxVal = 96.0f;
                } else if (kLower == "minlifetime") {
                    minVal = 2.0f;
                    maxVal = 16.0f;
                } else if (kLower == "maxlifetime") {
                    minVal = 3.0f;
                    maxVal = 24.0f;
                } else if (kLower == "giantcooldown") {
                    minVal = 30.0f;
                    maxVal = 240.0f;
                } else if (kLower == "animationspeed") {
                    minVal = 0.50f;
                    maxVal = 2.50f;
                } else if (kLower == "motionscale") {
                    minVal = 0.25f;
                    maxVal = 3.00f;
                } else if (kLower == "pickupradiusscale") {
                    minVal = 0.50f;
                    maxVal = 1.50f;
                } else if (kLower == "labelheight") {
                    minVal = 0.50f;
                    maxVal = 4.00f;
                } else if (kLower == "mergeradius") {
                    minVal = 0.50f;
                    maxVal = 6.00f;
                } else if (kLower == "horizontaloffset" || kLower == "verticaloffset") {
                    minVal = -1.25f;
                    maxVal = 1.25f;
                } else if (kLower == "bubblelifetime") {
                    minVal = 2.0f;
                    maxVal = 20.0f;
                } else if (kLower == "chatradius") {
                    minVal = 6.0f;
                    maxVal = 64.0f;
                } else if (kLower == "hiddenduration") {
                    if (mod->moduleId == "worldanalysis.Analysis Menu") {
                        minVal = 3.0f;
                        maxVal = 600.0f;
                    } else {
                        minVal = 2.0f;
                        maxVal = 20.0f;
                    }
                } else if (kLower == "resultduration") {
                    minVal = 3.0f;
                    maxVal = 60.0f;
                } else if (kLower == "duration") {
                    minVal = 0.25f;
                    maxVal = 10.0f;
                } else if (kLower == "transparency") {
                    minVal = 0.0f;
                    maxVal = 0.95f;
                } else if (kLower.find("opacity") != std::string::npos ||
                    kLower.find("color") != std::string::npos ||
                    kLower.find("alpha") != std::string::npos) {
                    maxVal = 1.0f;
                } else if (kLower.find("scale") != std::string::npos) {
                    minVal = 0.1f;
                    maxVal = 5.0f;
                } else if (kLower == "borderwidth") {
                    maxVal = 4.0f;
                } else if (kLower.find("width") != std::string::npos) {
                    maxVal = 1000.0f;
                } else if (kLower.find("position") != std::string::npos ||
                           kLower.find("posx") != std::string::npos ||
                           kLower.find("posy") != std::string::npos) {
                    maxVal = 2000.0f;
                } else if (kLower == "pathrange" || kLower == "healthbarrange" || kLower == "healthrange" || kLower == "inventoryrange" || kLower == "hungerrange" || kLower == "highlightrange" || kLower == "reachdisplayrange") {
                    minVal = 4.0f;
                    maxVal = 96.0f;
                } else if (kLower == "spawnlightrange" || kLower == "dangerradius") {
                    minVal = 4.0f;
                    maxVal = 48.0f;
                } else if (kLower == "lavaradius") {
                    minVal = 4.0f;
                    maxVal = 32.0f;
                } else if (kLower == "eventradius") {
                    minVal = 6.0f;
                    maxVal = 32.0f;
                } else if (kLower == "countrange") {
                    minVal = 8.0f;
                    maxVal = 96.0f;
                } else if (kLower == "buttonx" || kLower == "buttony" || kLower == "barx" || kLower == "bary") {
                    minVal = 0.0f;
                    maxVal = 100.0f;
                } else if (kLower == "buttonscale" || kLower == "barscale") {
                    minVal = 0.5f;
                    maxVal = 2.0f;
                } else if (kLower == "scanradius") {
                    minVal = 8.0f;
                    maxVal = 24.0f;
                } else if (kLower == "viewrange") {
                    minVal = 8.0f;
                    maxVal = 64.0f;
                } else if (kLower == "verticalrange") {
                    minVal = 6.0f;
                    maxVal = 24.0f;
                } else if (kLower == "pulsespeed") {
                    minVal = 8.0f;
                    maxVal = 80.0f;
                } else if (kLower == "pulseinterval") {
                    minVal = 0.35f;
                    maxVal = 12.0f;
                } else if (kLower == "pulsewidth") {
                    minVal = 0.35f;
                    maxVal = 4.0f;
                } else if (kLower == "markerduration") {
                    minVal = 1.0f;
                    maxVal = 10.0f;
                } else if (kLower == "pulsefaceduration") {
                    minVal = 0.35f;
                    maxVal = 4.0f;
                } else if (kLower == "slimechunkgridduration") {
                    minVal = 3.0f;
                    maxVal = 60.0f;
                } else if (kLower == "notificationradius") {
                    minVal = 8.0f;
                    maxVal = 96.0f;
                } else if (kLower == "notificationduration") {
                    minVal = 1.0f;
                    maxVal = 10.0f;
                } else if (kLower == "highlightduration") {
                    minVal = 1.0f;
                    maxVal = 10.0f;
                } else if (kLower == "calculationinterval") {
                    minVal = 1.0f;
                    maxVal = 12.0f;
                } else if (kLower == "cubesize" || kLower == "circlesize") {
                    minVal = 0.08f;
                    maxVal = 0.45f;
                } else if (kLower == "circlethickness") {
                    minVal = 2.0f;
                    maxVal = 7.0f;
                } else if (kLower == "surfaceopacity" || kLower == "fillopacity") {
                    minVal = 0.01f;
                    maxVal = 0.75f;
                } else if (kLower.find("range") != std::string::npos) {
                    maxVal = 180.0f;
                } else if (kLower.find("fov") != std::string::npos) {
                    minVal = 1.0f;
                    maxVal = 179.0f;
                } else if (kLower == "intensity") {
                    minVal = 0.10f;
                    maxVal = 1.0f;
                } else if (kLower.find("speed") != std::string::npos || kLower.find("strength") != std::string::npos) {
                    minVal = 0.05f;
                    maxVal = 1.0f;
                } else if (kLower.find("thick") != std::string::npos) {
                    minVal = 1.0f;
                    maxVal = 4.0f;
                }

                entry.min_value = std::to_string(minVal);
                entry.max_value = std::to_string(maxVal);
            } else if (v.is_string()) {
                std::string str = v.get<std::string>();
                if (str.find(',') != std::string::npos) {
                    entry.type = pl::modmenu::ConfigType::Radio;
                    size_t firstComma = str.find(',');
                    entry.default_value = str.substr(0, firstComma);
                    entry.min_value = str.substr(firstComma + 1);
                } else if (!str.empty() && str[0] == '#') {
                    entry.type = pl::modmenu::ConfigType::Color;
                    entry.default_value = str;
                } else {
                    entry.type = pl::modmenu::ConfigType::Text;
                    entry.default_value = str;
                }
            } else {
                continue;
            }
            configs.push_back(entry);
        }

                for (auto& entry : configs) {
            std::string k = entry.key;
            if (entry.type != pl::modmenu::ConfigType::Toggle) {
                std::string bestParent = "";
                for (const auto& parentCandidate : configs) {
                    if (parentCandidate.type == pl::modmenu::ConfigType::Toggle) {
                        std::string pKey = parentCandidate.key;
                        if (k.length() > pKey.length() && k.compare(0, pKey.length(), pKey) == 0) {
                            if (pKey.length() > bestParent.length()) {
                                bestParent = pKey;
                            }
                        }
                    }
                }
                if (!bestParent.empty()) entry.depends_on = bestParent;
            }
            builder.config(entry.key, entry.displayName, entry.type, entry.default_value, entry.min_value, entry.max_value, entry.depends_on);
        }

        const bool registered = builder.registerModule();
        if (registered && mod->moduleId == "worldanalysis.World Scan") {
            static_cast<WorldScanModule*>(mod)->registerLauncherButton();
        } else if (registered && mod->moduleId == "worldanalysis.Analyze") {
            static_cast<AnalyzeModule*>(mod)->registerLauncherButton();
        } else if (registered && mod->moduleId == "worldanalysis.Ocean Depth") {
            static_cast<OceanDepthModule*>(mod)->registerLauncherButton();
        }
    }
}
