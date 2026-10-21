#pragma once
#include <atomic>
#include <mutex>
#include <utility>

// The event owns this gate, not the capture object. A queued event can safely
// outlive its owner; shutdown closes admission and drains the active callback.
template<class Owner>
class CaptureCallbackGate {
public:
    explicit CaptureCallbackGate(Owner* owner) : owner_(owner) {}

    template<class Action>
    bool Invoke(Action&& action) {
        if (closing_.load()) return false;
        std::scoped_lock lock(mutex_);
        if (closing_.load() || !owner_) return false;
        std::forward<Action>(action)(*owner_);
        return true;
    }

    // Called by the owner, never from inside an admitted callback. No WinRT
    // Close or event revocation runs while holding this mutex.
    void StopAndWait() {
        closing_.store(true);
        std::scoped_lock lock(mutex_);
        owner_ = nullptr;
    }

private:
    std::atomic<bool> closing_{false};
    std::mutex mutex_;
    Owner* owner_;
};
