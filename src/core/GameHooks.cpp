#include "GameHooks.hpp"

#include "core/memory/Hooks.hpp"
#include <worldanalysis/events/EventBus.hpp>
#include <worldanalysis/memory/Signatures.hpp>
#include <worldanalysis/sdk/world/Actor.hpp>
#include <worldanalysis/sdk/client/ClientInstance.hpp>
#include <EGL/egl.h>
#include <dlfcn.h>
#include <array>
#include <atomic>
#include <mutex>

namespace worldanalysis::core::gamehooks {
namespace {
using namespace worldanalysis::events;
using worldanalysis::memory::SignatureId;

using NormalTickFn = void(*)(void*);
using ClientInstanceUpdateFn = void*(*)(void*, bool);
// onOpen()/destructors are single-this, void-return methods in the supplied
// 26.45 image.  An earlier generic eight-register prototype was unnecessary
// and made these global UI hooks harder to audit.
using ScreenFn = void(*)(void*);
using EglSwapBuffersFn = EGLBoolean(*)(EGLDisplay, EGLSurface);

NormalTickFn tickOriginal = nullptr;
ClientInstanceUpdateFn clientUpdateOriginal = nullptr;
ScreenFn containerOpenOriginal = nullptr;
ScreenFn containerCloseOriginal = nullptr;
ScreenFn chatOpenOriginal = nullptr;
ScreenFn chatCloseOriginal = nullptr;
EglSwapBuffersFn swapBuffersOriginal = nullptr;
std::atomic<void*> currentClientInstance = nullptr;
std::array<worldanalysis::hooks::Handle, 7> handles{};
std::size_t handleCount = 0;
std::mutex installMutex;
bool installed = false;

template <class Function>
bool hookSignature(SignatureId id, void* detour, Function** original) {
    const auto address = worldanalysis::memory::resolve(id);
    if (!address) return false;
    auto handle = worldanalysis::hooks::install(reinterpret_cast<void*>(address), detour,
                                                reinterpret_cast<void**>(original));
    if (!handle) return false;
    if (handleCount < handles.size()) handles[handleCount++] = handle;
    return true;
}

void tickDetour(void* actor) {
    auto* player = reinterpret_cast<worldanalysis::sdk::Player*>(actor);
    LocalPlayerPreTickEvent preEvent{player};
    bus().publish(preEvent);
    if (tickOriginal) tickOriginal(actor);
    LocalPlayerTickEvent postEvent{player};
    bus().publish(postEvent);
}

void* clientUpdateDetour(void* clientInstance, bool value) {
    if (clientInstance) currentClientInstance.store(clientInstance, std::memory_order_release);
    void* result = clientUpdateOriginal ? clientUpdateOriginal(clientInstance, value) : nullptr;
    ClientInstanceUpdateEvent event{reinterpret_cast<worldanalysis::sdk::ClientInstance*>(clientInstance)};
    bus().publish(event);
    return result;
}


void containerOpenDetour(void* a0) {
    // Publish only after Minecraft has initialized the screen controller.  The
    // Instore Viewer immediately reads controller-owned collection state, and
    // observing the object before the original open completed could expose an
    // uninitialized/stale manager for the first synchronization samples.
    if (containerOpenOriginal) containerOpenOriginal(a0);
    ScreenStateEvent event{ScreenKind::Container, ScreenPhase::Opened, a0};
    bus().publish(event);
}

void containerCloseDetour(void* a0) {
    ScreenStateEvent event{ScreenKind::Container, ScreenPhase::Closed, a0};
    bus().publish(event);
    if (containerCloseOriginal) containerCloseOriginal(a0);
}

void chatOpenDetour(void* a0) {
    ScreenStateEvent event{ScreenKind::Chat, ScreenPhase::Opened, a0};
    bus().publish(event);
    if (chatOpenOriginal) chatOpenOriginal(a0);
}

void chatCloseDetour(void* a0) {
    ScreenStateEvent event{ScreenKind::Chat, ScreenPhase::Closed, a0};
    bus().publish(event);
    if (chatCloseOriginal) chatCloseOriginal(a0);
}

EGLBoolean swapBuffersDetour(EGLDisplay display, EGLSurface surface) {
    if (eglGetCurrentContext() != EGL_NO_CONTEXT) {
        FrameEvent event;
        bus().publish(event);
    }
    return swapBuffersOriginal ? swapBuffersOriginal(display, surface) : EGL_FALSE;
}

bool hookEgl() {
    // Prefer the loaded EGL library, but fall back to the process symbol. Some
    // Android EGL loader stacks expose the callable entry through the global
    // namespace even when opening libEGL.so by name is not hookable.
    void* target = nullptr;
    auto egl = worldanalysis::hooks::openLibrary("libEGL.so");
    if (egl) target = reinterpret_cast<void*>(worldanalysis::hooks::symbol(egl, "eglSwapBuffers"));
    if (!target) target = dlsym(RTLD_DEFAULT, "eglSwapBuffers");
    if (!target) {
        if (egl) worldanalysis::hooks::closeLibrary(egl);
        return false;
    }
    auto handle = worldanalysis::hooks::install(target,
        reinterpret_cast<void*>(swapBuffersDetour), reinterpret_cast<void**>(&swapBuffersOriginal));
    if (egl) worldanalysis::hooks::closeLibrary(egl);
    if (!handle) return false;
    if (handleCount < handles.size()) handles[handleCount++] = handle;
    return true;
}
} // namespace

bool install() {
    std::lock_guard lock(installMutex);
    if (installed) return true;
    hookSignature(SignatureId::NormalTick, reinterpret_cast<void*>(tickDetour), &tickOriginal);
    hookSignature(SignatureId::ClientInstanceUpdate, reinterpret_cast<void*>(clientUpdateDetour), &clientUpdateOriginal);
    hookSignature(SignatureId::ContainerScreenControllerOpen, reinterpret_cast<void*>(containerOpenDetour), &containerOpenOriginal);
    hookSignature(SignatureId::ContainerScreenControllerDtor, reinterpret_cast<void*>(containerCloseDetour), &containerCloseOriginal);
    hookSignature(SignatureId::ChatScreenOpen, reinterpret_cast<void*>(chatOpenDetour), &chatOpenOriginal);
    hookSignature(SignatureId::ChatScreenDtor, reinterpret_cast<void*>(chatCloseDetour), &chatCloseOriginal);
    hookEgl();
    installed = tickOriginal != nullptr && clientUpdateOriginal != nullptr;
    return installed;
}

void uninstall() {
    std::lock_guard lock(installMutex);
    for (std::size_t i = 0; i < handleCount; ++i) {
        if (handles[i]) worldanalysis::hooks::remove(handles[i]);
        handles[i] = nullptr;
    }
    handleCount = 0;
    installed = false;
    currentClientInstance.store(nullptr, std::memory_order_release);
}

void* clientInstance() { return currentClientInstance.load(std::memory_order_acquire); }

} // namespace worldanalysis::core::gamehooks
