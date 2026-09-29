#pragma once

#include <worldanalysis/Api.hpp>
#include <functional>
#include <memory>
#include <utility>

namespace worldanalysis::events {

template <class Event>
class RuntimeListener {
public:
    RuntimeListener() = default;

    template <class Callback>
    explicit RuntimeListener(Callback&& callback, EventPriority priority = EventPriority::Normal, const api::ApiV1* runtime = nullptr) {
        subscribe(std::forward<Callback>(callback), priority, runtime);
    }

    RuntimeListener(const RuntimeListener&) = delete;
    RuntimeListener& operator=(const RuntimeListener&) = delete;

    RuntimeListener(RuntimeListener&& other) noexcept { moveFrom(std::move(other)); }

    RuntimeListener& operator=(RuntimeListener&& other) noexcept {
        if (this != &other) {
            reset();
            moveFrom(std::move(other));
        }
        return *this;
    }

    ~RuntimeListener() { reset(); }

    template <class Callback>
    bool subscribe(Callback&& callback, EventPriority priority = EventPriority::Normal, const api::ApiV1* runtime = nullptr) {
        reset();
        mRuntime = runtime ? runtime : api::find();
        if (!api::compatible(mRuntime) || !mRuntime->subscribe) return false;
        mState = std::make_unique<State>();
        mState->callback = std::forward<Callback>(callback);
        mSubscription = mRuntime->subscribe(Event::type, priority, dispatch, mState.get());
        if (mSubscription != 0) return true;
        mState.reset();
        mRuntime = nullptr;
        return false;
    }

    void reset() {
        if (mRuntime && mRuntime->unsubscribe && mSubscription) mRuntime->unsubscribe(mSubscription);
        mSubscription = 0;
        mState.reset();
        mRuntime = nullptr;
    }

    explicit operator bool() const noexcept { return mSubscription != 0; }
    std::uint64_t id() const noexcept { return mSubscription; }

private:
    struct State {
        std::function<void(Event&)> callback;
    };

    static void dispatch(EventType type, void* payload, void* userData) {
        if (type != Event::type || !payload || !userData) return;
        auto* state = static_cast<State*>(userData);
        state->callback(*static_cast<Event*>(payload));
    }

    void moveFrom(RuntimeListener&& other) noexcept {
        mRuntime = other.mRuntime;
        mSubscription = other.mSubscription;
        mState = std::move(other.mState);
        other.mRuntime = nullptr;
        other.mSubscription = 0;
    }

    const api::ApiV1* mRuntime = nullptr;
    std::uint64_t mSubscription = 0;
    std::unique_ptr<State> mState;
};

}
