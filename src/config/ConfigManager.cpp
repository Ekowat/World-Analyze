#include "ConfigManager.hpp"
#include "modules/ModuleRegistry.hpp"
#include <filesystem>
#include <fstream>

namespace worldanalysis::config {

ConfigManager::~ConfigManager() {
    flush();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
    }
    m_condition.notify_all();
    if (m_worker.joinable()) m_worker.join();
}

std::string ConfigManager::getConfigPath() const {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_configPath.empty()) return m_configPath;
    }
    return "/sdcard/games/WorldAnalysis/config.json";
}

void ConfigManager::setConfigPath(const std::string& path) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_configPath = path;
}

std::string ConfigManager::getDataPath(const std::string& filename) const {
    std::filesystem::path config(getConfigPath());
    auto parent = config.parent_path();
    if (parent.empty()) parent = std::filesystem::path("/sdcard/games/WorldAnalysis");
    return (parent / filename).string();
}

void ConfigManager::load() {
    std::string path = getConfigPath();
    // One-time compatibility with pre-rebrand local installs. World Analysis
    // writes only to its new directory, but can seed settings from TechAnalysis
    // if the new config does not exist yet.
    if (!std::filesystem::exists(path)) {
        const std::filesystem::path legacy("/sdcard/games/TechAnalysis/config.json");
        if (std::filesystem::exists(legacy)) path = legacy.string();
        else {
            save();
            flush();
            return;
        }
    }

    try {
        std::ifstream inFile(path);
        nlohmann::json j;
        inFile >> j;

        if (j.contains("Modules")) {
            auto& modulesObj = j["Modules"];
            for (auto* mod : ModuleRegistry::get().modules()) {
                if (modulesObj.contains(mod->name)) mod->loadConfig(modulesObj[mod->name]);
            }
        }
    } catch (...) {
    }
}

void ConfigManager::save() {
    nlohmann::json j;

    for (auto* mod : ModuleRegistry::get().modules()) {
        nlohmann::json modJson;
        mod->saveConfig(modJson);
        j["Modules"][mod->name] = std::move(modJson);
    }

    std::string path = getConfigPath();
    std::string payload = j.dump();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) return;
        ensureWorkerLocked();
        m_pendingPath = std::move(path);
        m_pendingPayload = std::move(payload);
        m_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
        ++m_queuedGeneration;
        m_hasPending = true;
    }
    m_condition.notify_all();
}

void ConfigManager::flush() {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_worker.joinable() || !m_hasPending) return;
    const std::uint64_t targetGeneration = m_queuedGeneration;
    m_deadline = std::chrono::steady_clock::now();
    m_condition.notify_all();
    m_condition.wait(lock, [&] {
        return m_completedGeneration >= targetGeneration || m_stopping;
    });
}

void ConfigManager::ensureWorkerLocked() {
    if (!m_worker.joinable()) m_worker = std::thread(&ConfigManager::workerLoop, this);
}

void ConfigManager::workerLoop() {
    std::unique_lock<std::mutex> lock(m_mutex);
    while (true) {
        m_condition.wait(lock, [&] { return m_stopping || m_hasPending; });
        if (m_stopping && !m_hasPending) break;

        const std::uint64_t generation = m_queuedGeneration;
        const auto deadline = m_deadline;
        if (!m_stopping && m_condition.wait_until(lock, deadline, [&] {
            return m_stopping || m_queuedGeneration != generation || m_deadline != deadline;
        })) {
            continue;
        }

        std::string path = std::move(m_pendingPath);
        std::string payload = std::move(m_pendingPayload);
        m_hasPending = false;
        lock.unlock();
        writePayload(path, payload);
        lock.lock();
        if (m_completedGeneration < generation) m_completedGeneration = generation;
        m_condition.notify_all();
    }
}

void ConfigManager::writePayload(const std::string& pathValue, const std::string& payload) {
    std::filesystem::path path(pathValue);
    std::filesystem::path tempPath = path;
    tempPath += ".tmp";
    try {
        if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
        {
            std::ofstream outFile(tempPath, std::ios::binary | std::ios::trunc);
            outFile.write(payload.data(), static_cast<std::streamsize>(payload.size()));
            outFile.flush();
            if (!outFile) return;
        }
        std::error_code ec;
        std::filesystem::rename(tempPath, path, ec);
        if (ec) {
            std::filesystem::remove(path, ec);
            ec.clear();
            std::filesystem::rename(tempPath, path, ec);
            if (ec) std::filesystem::remove(tempPath, ec);
        }
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove(tempPath, ec);
    }
}

}
