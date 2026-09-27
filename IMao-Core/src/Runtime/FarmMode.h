#pragma once
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The farming mode ("刷怪采集"): while the player follows a planned route, arriving at a
// target marks it complete by itself. Only the daily-refresh categories take part — the
// one-off collectibles keep needing a deliberate mark — and everything this mode writes
// goes into FarmCompletionStore rather than the synchronized ledger.
namespace FarmMode {
using Clock = std::chrono::steady_clock;

// A point counts as reached while its direction indicator is this far from the player
// arrow, in minimap screen pixels. Same unit and bounds as the two trigger ranges the
// completion and guide keys already use, so the player tunes all three the same way.
inline constexpr int DefaultRangePixels = 30;
inline constexpr int MinimumRangePixels = 5;
inline constexpr int MaximumRangePixels = 120;

// One frame inside the range is not a decision: the player is walking past the point,
// still finishing the fight, or the icon merely drifted under the arrow.
inline constexpr auto Dwell = std::chrono::milliseconds(500);

class Range {
public:
    static int Pixels() { return pixels_.load(std::memory_order_relaxed); }
    static void Validate(int pixels) {
        if (pixels < MinimumRangePixels || pixels > MaximumRangePixels)
            throw std::invalid_argument("刷怪采集触发范围必须在 5–120 像素之间");
    }
    static void Apply(int pixels) { Validate(pixels); pixels_.store(pixels, std::memory_order_relaxed); }
private:
    inline static std::atomic<int> pixels_{DefaultRangePixels};
};

// Which targets have stayed inside the range long enough to be marked. A key that leaves
// the range — or that the observation stops reporting — starts its dwell over, so a
// target can never be marked from evidence gathered on an earlier pass. A marked key is
// forgotten immediately, so the same point is never written twice from one approach.
class Confirmation {
public:
    std::vector<std::string> Observe(const std::vector<std::string>& inRange, Clock::time_point now) {
        std::vector<std::string> due;
        std::unordered_set<std::string> current(inRange.begin(), inRange.end());
        for (auto it = since_.begin(); it != since_.end();) {
            if (!current.contains(it->first)) it = since_.erase(it);
            else ++it;
        }
        for (const auto& key : inRange) {
            const auto [it, inserted] = since_.try_emplace(key, now);
            if (inserted || now < it->second) { it->second = now; continue; }
            if (now - it->second >= Dwell) due.push_back(key);
        }
        for (const auto& key : due) since_.erase(key);
        return due;
    }
    void Reset() { since_.clear(); }
    std::size_t Tracked() const { return since_.size(); }
private:
    std::unordered_map<std::string, Clock::time_point> since_;
};
} // namespace FarmMode
