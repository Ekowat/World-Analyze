#pragma once

#include "Module.hpp"
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

class ModuleRegistry {
public:
    static ModuleRegistry& get();

    template <class T, class... Args>
    T& emplace(Args&&... args) {
        auto module = std::make_unique<T>(std::forward<Args>(args)...);
        auto* raw = module.get();
        mById.emplace(raw->moduleId, raw);
        mView.push_back(raw);
        mOwned.push_back(std::move(module));
        return *raw;
    }

    Module* find(std::string_view id) const;
    const std::vector<Module*>& modules() const;
    void initialize();
    void onFrame();
    bool onMouseEvent(int button, bool isDown);
    bool onKeyInput(int keyCode, unsigned int unicodeChar, bool isDown);
    bool onTouchInput(int action, int pointerId, float x, float y);
    void setKeybindBlocked(bool blocked);
    bool keybindBlocked() const;

private:
    struct StringHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view value) const noexcept {
            return std::hash<std::string_view>{}(value);
        }
    };

    std::vector<std::unique_ptr<Module>> mOwned;
    std::vector<Module*> mView;
    std::unordered_map<std::string, Module*, StringHash, std::equal_to<>> mById;
    bool mInitialized = false;
    bool mKeybindBlocked = false;
};

void registerAllModules();
