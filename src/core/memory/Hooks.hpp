#pragma once

#include <cstddef>
#include <cstdint>
#include <dlfcn.h>
#include <new>
#include <pl/memory/Hook.hpp>

namespace worldanalysis::hooks {

using LibraryHandle = void*;

struct State {
    void* target;
    void* detour;
};

using Handle = State*;

inline LibraryHandle openLibrary(const char* libraryName) {
    if (!libraryName) return nullptr;
    void* handle = dlopen(libraryName, RTLD_NOW | RTLD_NOLOAD);
    return handle ? handle : dlopen(libraryName, RTLD_NOW);
}

inline int closeLibrary(LibraryHandle handle) {
    return handle ? dlclose(handle) : 0;
}

inline std::uintptr_t symbol(LibraryHandle handle, const char* name) {
    if (!handle || !name) return 0;
    return reinterpret_cast<std::uintptr_t>(dlsym(handle, name));
}

inline Handle install(void* target, void* detour, void** original) {
    if (!target || !detour) return nullptr;
    if (pl::memory::hook(target, detour, original) != 0) return nullptr;
    auto* state = new (std::nothrow) State{target, detour};
    if (state) return state;
    pl::memory::unhook(target, detour);
    return nullptr;
}

inline void remove(Handle hook) {
    if (!hook) return;
    pl::memory::unhook(hook->target, hook->detour);
    delete hook;
}

}
