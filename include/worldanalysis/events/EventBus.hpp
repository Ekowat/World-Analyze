#pragma once

#include <worldanalysis/events/Events.hpp>
#include <worldanalysis/Export.hpp>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace worldanalysis::events {

using Subscription = std::uint64_t;

class EventBus {
public:
    using RawCallback = std::function<void(void*)>;

    Subscription subscribeRaw(EventType type, RawCallback callback, EventPriority priority = EventPriority::Normal) {
        if (!callback) return 0;
        std::unique_lock lock(mMutex);
        const auto id = mNextId++;
        auto& listeners = mListeners[type];
        listeners.push_back({id, priority, std::move(callback)});
        std::stable_sort(listeners.begin(), listeners.end(), [](const Entry& left, const Entry& right) {
            return static_cast<std::int32_t>(left.priority) > static_cast<std::int32_t>(right.priority);
        });
        return id;
    }

    template <class Event, class Callback>
    Subscription subscribe(Callback&& callback, EventPriority priority = EventPriority::Normal) {
        return subscribeRaw(Event::type, [handler = std::forward<Callback>(callback)](void* payload) mutable {
            handler(*static_cast<Event*>(payload));
        }, priority);
    }

    void unsubscribe(Subscription id) {
        if (id == 0) return;
        std::unique_lock lock(mMutex);
        for (auto& [type, listeners] : mListeners) {
            const auto it = std::remove_if(listeners.begin(), listeners.end(), [id](const Entry& entry) { return entry.id == id; });
            listeners.erase(it, listeners.end());
        }
    }

    void publishRaw(EventType type, void* payload) {
        std::vector<Entry> snapshot;
        {
            std::shared_lock lock(mMutex);
            const auto it = mListeners.find(type);
            if (it == mListeners.end()) return;
            snapshot = it->second;
        }
        for (auto& entry : snapshot) entry.callback(payload);
    }

    template <class Event>
    void publish(Event& event) {
        publishRaw(Event::type, &event);
    }

    void clear() {
        std::unique_lock lock(mMutex);
        mListeners.clear();
    }

private:
    struct Entry {
        Subscription id;
        EventPriority priority;
        RawCallback callback;
    };

    struct EventTypeHash {
        std::size_t operator()(EventType value) const noexcept {
            return static_cast<std::size_t>(value);
        }
    };

    std::shared_mutex mMutex;
    std::unordered_map<EventType, std::vector<Entry>, EventTypeHash> mListeners;
    std::uint64_t mNextId = 1;
};

WORLDANALYSIS_API EventBus& bus();

}
