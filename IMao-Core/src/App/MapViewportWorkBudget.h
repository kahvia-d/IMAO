#pragma once
#include <cstddef>
#include <cstdint>
#include <tuple>

// Worker-owned traversal position only. No image, match, location or negative
// evidence survives a request. Each resumed group checks the current frame afresh.
class MapViewportWorkBudget {
public:
    size_t Begin(uint64_t session, uint64_t ui, uint64_t viewport, size_t groups) {
        const auto context = std::make_tuple(session, ui, viewport, groups);
        if (!initialized_ || context != context_) { context_ = context; next_ = 0; initialized_ = true; }
        groups_ = groups;
        return groups_ ? next_ % groups_ : 0;
    }
    void Completed(size_t group) { next_ = groups_ ? (group + 1) % groups_ : 0; }
    void Reset() { initialized_ = false; next_ = 0; }
    static bool ShouldYield(bool global, size_t completedGroups, double elapsedMilliseconds) {
        return global && completedGroups > 0 && elapsedMilliseconds >= 2000.0;
    }
private:
    std::tuple<uint64_t, uint64_t, uint64_t, size_t> context_{};
    bool initialized_ = false;
    size_t next_ = 0, groups_ = 0;
};
