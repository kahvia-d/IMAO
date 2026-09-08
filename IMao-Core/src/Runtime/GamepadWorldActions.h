#pragma once
#include "GamepadContext.h"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

// IPC submits only an immutable intent. The App's existing key-monitoring
// thread revalidates and consumes it against its currently presented minimap.
// Every call supplies a new GamepadContextSnapshot::Read and a live foreground
// check; retaining an old View is not a substitute for reading its expiry gate.
class GamepadWorldActions {
public:
    using Clock = GamepadContextSnapshot::Clock;
    enum class Action { CompleteCurrent, ToggleGuide };
    struct Request {
        Action action = Action::CompleteCurrent;
        std::string profileId;
        std::uint64_t contextGeneration = 0, gameHwnd = 0;
    };
    static constexpr auto MaximumAge = std::chrono::milliseconds(250);
    static constexpr std::size_t MaximumPending = 16;

    static GamepadWorldActions& Shared() { static GamepadWorldActions value; return value; }

    static std::optional<Action> ParseAction(const std::string& value) {
        if (value == "completeCurrent") return Action::CompleteCurrent;
        if (value == "toggleGuide") return Action::ToggleGuide;
        return std::nullopt;
    }

    bool Enqueue(const Request& request, const GamepadContextSnapshot::View& current,
        bool gameFocused, Clock::time_point now = Clock::now()) {
        std::scoped_lock lock(mutex_);
        DiscardInvalidLocked(current, gameFocused, now);
        if (!Matches(request, current, gameFocused) || pending_.size() >= MaximumPending) return false;
        pending_.push_back({request, current.session, current.gameProcessId, now});
        return true;
    }

    std::optional<Request> Take(const GamepadContextSnapshot::View& current,
        bool gameFocused, Clock::time_point now = Clock::now()) {
        std::scoped_lock lock(mutex_);
        DiscardInvalidLocked(current, gameFocused, now);
        if (pending_.empty()) return std::nullopt;
        const auto request = std::move(pending_.front().request);
        pending_.pop_front();
        return request;
    }

    void Clear() { std::scoped_lock lock(mutex_); pending_.clear(); }

private:
    struct Pending {
        Request request;
        std::uint64_t session;
        std::uint32_t processId;
        Clock::time_point receivedAt;
    };
    static bool Matches(const Request& request, const GamepadContextSnapshot::View& current, bool gameFocused) {
        const bool knownAction = request.action == Action::CompleteCurrent || request.action == Action::ToggleGuide;
        return knownAction && gameFocused && current.running && current.observable && current.gameplay && !current.bigMap &&
            current.session != 0 && current.gameHwnd != 0 && current.gameProcessId != 0 && !current.profileId.empty() &&
            request.gameHwnd == current.gameHwnd && request.contextGeneration == current.generation &&
            request.profileId == current.profileId;
    }
    void DiscardInvalidLocked(const GamepadContextSnapshot::View& current, bool gameFocused, Clock::time_point now) {
        for (auto iterator = pending_.begin(); iterator != pending_.end();) {
            if (!Matches(iterator->request, current, gameFocused) || iterator->session != current.session ||
                iterator->processId != current.gameProcessId || now < iterator->receivedAt ||
                now - iterator->receivedAt >= MaximumAge)
                iterator = pending_.erase(iterator);
            else ++iterator;
        }
    }
    std::mutex mutex_;
    std::deque<Pending> pending_;
};
