#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>

// Scheduling evidence only. This policy never accepts or publishes a position.
class VisualRecoveryHints {
public:
    using Clock = std::chrono::steady_clock;
    void SetEpoch(std::uint64_t session, std::uint64_t generation) {
        if (session_ == session && generation_ == generation) return;
        session_ = session;
        generation_ = generation;
        failed_.clear();
        timeouts_.clear();
        globalUntil_ = {};
        active_ = 0;
        scope_.clear();
    }
    bool PreserveGlobal(Clock::time_point now) const { return now < globalUntil_; }
    bool AllowScope(const std::string& scope, Clock::time_point now) const {
        return !scope.empty() && !PreserveGlobal(now) && !failed_.contains(scope);
    }
    void Submitted(std::uint64_t request, std::string scope, bool bounded) {
        active_ = request;
        scope_ = std::move(scope);
        bounded_ = bounded;
    }
    void Complete(std::uint64_t request, bool incomplete, bool rejected,
        bool hasCandidate, Clock::time_point now) {
        if (request != active_ || active_ == 0) return;
        active_ = 0;
        if (!scope_.empty() && rejected && !incomplete) failed_.insert(scope_);
        // A timed-out scope is inconclusive. Give the global search a continuous
        // opportunity before retrying. A global candidate gets its existing
        // two-second independent-frame confirmation window.
        if (!scope_.empty() && incomplete) {
            auto& attempts = timeouts_[scope_];
            attempts = std::min(attempts + 1, 11u);
            globalUntil_ = now + std::chrono::seconds(1u << attempts);
        }
        else if ((!scope_.empty() && rejected) ||
            (!bounded_ && hasCandidate && !incomplete && !rejected))
            globalUntil_ = now + std::chrono::seconds(2);
        scope_.clear();
    }
private:
    std::uint64_t session_ = 0, generation_ = 0, active_ = 0;
    bool bounded_ = false;
    std::string scope_;
    std::unordered_set<std::string> failed_;
    std::unordered_map<std::string, unsigned> timeouts_;
    Clock::time_point globalUntil_{};
};
