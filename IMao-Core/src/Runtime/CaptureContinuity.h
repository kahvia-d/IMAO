#pragma once
#include <chrono>

namespace CaptureContinuity {
using Clock = std::chrono::steady_clock;
inline bool Fresh(bool hasFrame, Clock::time_point capturedAt, Clock::time_point now,
    std::chrono::milliseconds maximumAge = std::chrono::milliseconds(250)) {
    return hasFrame && capturedAt != Clock::time_point{} && now >= capturedAt && now - capturedAt < maximumAge;
}
// Context lifetime is independent of overlay pixel freshness.
inline bool Retain(bool hasFrame, Clock::time_point capturedAt, Clock::time_point now) {
    return Fresh(hasFrame,capturedAt,now,std::chrono::milliseconds(750));
}
inline bool CanService(bool focused, bool invalidationPending, bool hasFrame,
    Clock::time_point capturedAt, Clock::time_point now) {
    return focused && !invalidationPending && Retain(hasFrame,capturedAt,now);
}
}
