#include "ModuleRegistry.hpp"
#include "visual/determine.hpp"
#include "visual/instoreviewer.hpp"
#include "visual/systemnotifications.hpp"
#include "visual/updateviewer.hpp"
#include "visual/techvfx.hpp"
#include "visual/analyze.hpp"
#include "visual/analysismenu.hpp"
#include "visual/mobcounter.hpp"
#include "visual/mobpathfinding.hpp"
#include "visual/worldscan.hpp"
#include "visual/oceandepth.hpp"

ModuleRegistry& ModuleRegistry::get() {
    static ModuleRegistry registry;
    return registry;
}

Module* ModuleRegistry::find(std::string_view id) const {
    const auto it = mById.find(id);
    return it == mById.end() ? nullptr : it->second;
}

const std::vector<Module*>& ModuleRegistry::modules() const { return mView; }

void ModuleRegistry::initialize() {
    if (mInitialized) return;
    for (auto* module : mView) module->onInit();
    mInitialized = true;
}

void ModuleRegistry::onFrame() {
    for (auto* module : mView) if (module->enabled) module->onFrame();
}

bool ModuleRegistry::onMouseEvent(int button, bool isDown) {
    bool consumed = false;
    for (auto* module : mView) if (module->onMouseEvent(button, isDown)) consumed = true;
    return consumed;
}

bool ModuleRegistry::onKeyInput(int keyCode, unsigned int unicodeChar, bool isDown) {
    bool consumed = false;
    for (auto* module : mView) if (module->onKeyInput(keyCode, unicodeChar, isDown)) consumed = true;
    return consumed;
}

bool ModuleRegistry::onTouchInput(int action, int pointerId, float x, float y) {
    bool consumed = false;
    for (auto* module : mView) if (module->onTouchInput(action, pointerId, x, y)) consumed = true;
    return consumed;
}

void ModuleRegistry::setKeybindBlocked(bool blocked) { mKeybindBlocked = blocked; }
bool ModuleRegistry::keybindBlocked() const { return mKeybindBlocked; }

void registerAllModules() {
    auto& registry = ModuleRegistry::get();
    if (!registry.modules().empty()) return;
    registry.emplace<DetermineModule>();
    registry.emplace<InstoreViewerModule>();
    registry.emplace<SystemNotificationsModule>();
    registry.emplace<UpdateViewerModule>();
    registry.emplace<TechVFXModule>();
    registry.emplace<AnalyzeModule>();
    registry.emplace<AnalysisMenuModule>();
    registry.emplace<MobCounterModule>();
    registry.emplace<MobPathfindingModule>();
    registry.emplace<WorldScanModule>();
    registry.emplace<OceanDepthModule>();
}
