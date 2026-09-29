#pragma once

#include <nlohmann/json.hpp>
#include "core/ui/ColorUtil.hpp"
#include <string>
#include <algorithm>
#include <utility>

class Module {
public:
    const char* name;
    const char* description;
    std::string moduleId;
    bool masterEnabled = false;
    bool keybindActive = true;
    bool enabled = false;
    bool showInMenu = true;
    bool hideInHudEditor = false;
    int keybind = 0;

    // Shared visual theming. #FFFFFF preserves each module's legacy colors;
    // choosing any other RGB value tints foreground world/UI lines and text.
    std::string color = "#FFFFFF";
    bool exposeBackgroundStyle = false;
    std::string backgroundColor = "#000000";
    float backgroundOpacity = 1.0f;

    Module(const char* n, const char* d) : name(n), description(d), moduleId(std::string("worldanalysis.") + n) {}
    virtual ~Module() = default;

    virtual void onInit()     {}
    virtual void onEnable()   {}
    virtual void onDisable()  {}
    virtual void onFrame()    {}
    virtual bool onMouseEvent(int button, bool isDown) { return false; }
    virtual bool onKeyInput(int keyCode, unsigned int unicodeChar, bool isDown) { (void)keyCode; (void)unicodeChar; (void)isDown; return false; }
    virtual bool onTouchInput(int action, int pointerId, float x, float y) { (void)action; (void)pointerId; (void)x; (void)y; return false; }
    
    virtual void onKeybindEvent(const std::string& key, bool isDown) {
        if (key == "keybind" && isDown) {
            setKeybindActive(!keybindActive);
        }
    }

    void setMasterEnabled(bool state) {
        if (masterEnabled == state) return;
        masterEnabled = state;
        updateEnabledState();
    }

    void setKeybindActive(bool state) {
        if (keybindActive == state) return;
        keybindActive = state;
        updateEnabledState();
    }

    void updateEnabledState() {
        bool newState = masterEnabled && keybindActive;
        if (newState != enabled) {
            enabled = newState;
            if (enabled) onEnable();
            else onDisable();
        }
    }

    virtual void loadConfig(const nlohmann::json& j) {
        if (j.contains("keybind")) keybind = j["keybind"].get<int>();
        if (j.contains("color") && j["color"].is_string()) color = worldanalysis::ui::normalizeColorString(j["color"].get<std::string>(), color);
        if (exposeBackgroundStyle) {
            if (j.contains("backgroundColor") && j["backgroundColor"].is_string())
                backgroundColor = worldanalysis::ui::normalizeColorString(j["backgroundColor"].get<std::string>(), backgroundColor);
            backgroundOpacity = std::clamp(j.value("backgroundOpacity", backgroundOpacity), 0.0f, 1.0f);
        }
        if (j.contains("keybindActive")) {
            keybindActive = j["keybindActive"].get<bool>();
            updateEnabledState();
        }
        if (j.contains("masterEnabled")) {
            masterEnabled = j["masterEnabled"].get<bool>();
            updateEnabledState();
        }
    }

    virtual void saveConfig(nlohmann::json& j) {
        j["keybind"] = keybind;
        color = worldanalysis::ui::normalizeColorString(color);
        j["color"] = color;
        if (exposeBackgroundStyle) {
            backgroundColor = worldanalysis::ui::normalizeColorString(backgroundColor, "#000000");
            j["backgroundColor"] = backgroundColor;
            j["backgroundOpacity"] = std::clamp(backgroundOpacity, 0.0f, 1.0f);
        }
        j["keybindActive"] = keybindActive;
        j["masterEnabled"] = masterEnabled;
    }
};
