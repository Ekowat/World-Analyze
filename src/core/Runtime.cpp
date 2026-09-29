#include "Runtime.hpp"
#include "GameHooks.hpp"
#include "config/ConfigManager.hpp"
#include "launcher/ModuleMenu.hpp"
#include "modules/ModuleRegistry.hpp"
#include "core/memory/Hooks.hpp"
#include <worldanalysis/events/EventBus.hpp>
#include <worldanalysis/memory/Signatures.hpp>
#include <pl/Input.hpp>
#include <atomic>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <mutex>
#include <unistd.h>

namespace worldanalysis::core {
namespace {
std::atomic_bool enabled = false;
std::atomic_bool resolved = false;
std::atomic_bool installed = false;
std::mutex resolveMutex;
std::mutex installMutex;
thread_local bool resolvingFromDlopen = false;
void* (*dlopenOriginal)(const char*, int) = nullptr;
worldanalysis::hooks::Handle dlopenHook = nullptr;
bool eventsWired = false;
bool modulesPrepared = false;
int containerDepth = 0;
int chatDepth = 0;

class ResolveGuard {
public:
    ResolveGuard() : mPrevious(resolvingFromDlopen) { resolvingFromDlopen = true; }
    ~ResolveGuard() { resolvingFromDlopen = mPrevious; }
private:
    bool mPrevious;
};

void* dlopenDetour(const char* filename, int flags) {
    void* handle = dlopenOriginal ? dlopenOriginal(filename, flags) : nullptr;
    if (handle && filename && std::strstr(filename, "libminecraftpe.so") && !resolvingFromDlopen) {
        Runtime::get().minecraftLoaded();
    }
    return handle;
}
}

Runtime& Runtime::get() {
    static Runtime runtime;
    return runtime;
}

const std::filesystem::path& Runtime::resourceDirectory() const noexcept {
    return mResourceDirectory;
}

bool Runtime::launcherContext() const {
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) return false;
    char command[256]{};
    const auto size = read(fd, command, sizeof(command) - 1);
    close(fd);
    if (size <= 0) return false;
    return std::strcmp(command, "org.levimc.launcher") == 0
        || std::strcmp(command, "org.levimc.launcher:minecraft") == 0
        || std::strcmp(command, "com.mojang.minecraftpe") == 0;
}

bool Runtime::resolveSignatures() {
    std::lock_guard lock(resolveMutex);
    if (resolved.load(std::memory_order_acquire)) return true;
    ResolveGuard guard;
    const bool ok = worldanalysis::memory::resolveAll("libminecraftpe.so");
    resolved.store(ok, std::memory_order_release);
    return ok;
}

void Runtime::wireEvents() {
    if (eventsWired) return;
    eventsWired = true;
    using namespace worldanalysis::events;
    bus().subscribe<FrameEvent>([](auto&) { ModuleRegistry::get().onFrame(); });
    bus().subscribe<ScreenStateEvent>([](auto& event) {
        int& depth = event.screen == ScreenKind::Container ? containerDepth : chatDepth;
        if (event.phase == ScreenPhase::Opened) ++depth;
        else if (depth > 0) --depth;
        ModuleRegistry::get().setKeybindBlocked(containerDepth > 0 || chatDepth > 0);
    });
    pl::input::registerMouseCallback([](const pl::input::MouseEvent& input) {
        return ModuleRegistry::get().onMouseEvent(input.button, input.isDown);
    });
    pl::input::registerKeyCallback([](const pl::input::KeyEvent& input) {
        return ModuleRegistry::get().onKeyInput(input.keyCode, input.unicodeChar, input.isKeyDown);
    });
    pl::input::registerTouchCallback([](const pl::input::TouchEvent& input) {
        return ModuleRegistry::get().onTouchInput(input.action, input.pointerId, input.x, input.y);
    });
}

bool Runtime::install() {
    std::lock_guard lock(installMutex);
    if (installed.load(std::memory_order_acquire)) return true;
    if (!resolved.load(std::memory_order_acquire) && !resolveSignatures()) return false;
    if (!gamehooks::install()) return false;
    wireEvents();
    ModuleRegistry::get().initialize();
    // Restore the original init-before-enable ordering for persisted/early toggles.
    for (auto* module : ModuleRegistry::get().modules()) {
        if (module->enabled) module->onEnable();
    }
    installed.store(true, std::memory_order_release);
    return true;
}

void Runtime::minecraftLoaded() {
    if (!resolveSignatures()) return;
    if (enabled.load(std::memory_order_acquire)) install();
}

bool Runtime::load(pl::mod::ModContext& context) {
    mResourceDirectory = context.resourceDir();
    worldanalysis::config::ConfigManager::get().setConfigPath((context.configDir() / "config.json").string());
    if (!launcherContext()) return true;
    void* minecraft = dlopen("libminecraftpe.so", RTLD_NOW | RTLD_NOLOAD);
    if (minecraft) {
        resolveSignatures();
        dlclose(minecraft);
        return true;
    }
    worldanalysis::hooks::LibraryHandle libdl = worldanalysis::hooks::openLibrary("libdl.so");
    if (!libdl) return true;
    void* symbol = reinterpret_cast<void*>(worldanalysis::hooks::symbol(libdl, "dlopen"));
    if (symbol) dlopenHook = worldanalysis::hooks::install(symbol, reinterpret_cast<void*>(dlopenDetour), reinterpret_cast<void**>(&dlopenOriginal));
    worldanalysis::hooks::closeLibrary(libdl);
    return true;
}

bool Runtime::enable(pl::mod::ModContext& context) {
    // Register in the loader lifecycle, before Minecraft signature/hook readiness.
    // Explicit ownership also works when Minecraft finishes loading on another thread.
    registerAllModules();
    if (!modulesPrepared) {
        worldanalysis::config::ConfigManager::get().load();
        modulesPrepared = true;
    }
    registerModulesWithLauncher(context.id());
    enabled.store(true, std::memory_order_release);
    if (!launcherContext()) return true;
    if (!resolved.load(std::memory_order_acquire)) {
        void* minecraft = dlopen("libminecraftpe.so", RTLD_NOW | RTLD_NOLOAD);
        if (!minecraft) return true;
        resolveSignatures();
        dlclose(minecraft);
    }
    install();
    return true;
}

bool Runtime::disable(pl::mod::ModContext&) {
    enabled.store(false, std::memory_order_release);
    worldanalysis::config::ConfigManager::get().flush();
    return true;
}

bool Runtime::unload(pl::mod::ModContext&) {
    enabled.store(false, std::memory_order_release);
    worldanalysis::config::ConfigManager::get().flush();
    return true;
}

}
