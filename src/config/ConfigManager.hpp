#pragma once

#include <nlohmann/json.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace worldanalysis::config {

class ConfigManager {
public:
    static ConfigManager& get() {
        static ConfigManager instance;
        return instance;
    }

    void load();
    void save();
    void flush();

    std::string getConfigPath() const;
    void setConfigPath(const std::string& path);
    std::string getDataPath(const std::string& filename) const;

private:
    ConfigManager() = default;
    ~ConfigManager();

    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    void ensureWorkerLocked();
    void workerLoop();
    static void writePayload(const std::string& path, const std::string& payload);

    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    std::thread m_worker;
    std::string m_configPath;
    std::string m_pendingPath;
    std::string m_pendingPayload;
    std::chrono::steady_clock::time_point m_deadline{};
    std::uint64_t m_queuedGeneration = 0;
    std::uint64_t m_completedGeneration = 0;
    bool m_hasPending = false;
    bool m_stopping = false;
};

}
