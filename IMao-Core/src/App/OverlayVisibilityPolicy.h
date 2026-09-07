#pragma once

#include "MapUiStateController.h"
#include <chrono>
#include <cstdint>

// Visibility is a property of the latest observed game UI, not of the last
// successful location. This small snapshot can revoke a published marker frame
// without waiting for the localization worker or clearing its recovery hints.
struct OverlayVisibilityFrame {
    using Clock = std::chrono::steady_clock;
    std::uint64_t frameId = 0;
    std::uint64_t visibleSinceFrame = 0;
    Clock::time_point capturedAt{};
    std::chrono::milliseconds maximumAge{500};
    bool mapVisible = false;
    bool minimapVisible = false;

    bool Fresh(Clock::time_point now = Clock::now()) const {
        return frameId != 0 && now >= capturedAt && now - capturedAt < maximumAge;
    }

    bool AllowsMap(std::uint64_t markerFrame, Clock::time_point now = Clock::now()) const {
        return mapVisible && markerFrame >= visibleSinceFrame && Fresh(now);
    }

    bool AllowsMinimap(std::uint64_t markerFrame, Clock::time_point now = Clock::now()) const {
        return minimapVisible && markerFrame >= visibleSinceFrame && Fresh(now);
    }
};

class OverlayVisibilityPolicy {
public:
    OverlayVisibilityFrame Observe(std::uint64_t frameId,
        OverlayVisibilityFrame::Clock::time_point capturedAt,
        std::chrono::milliseconds maximumAge, MapUiState stableState,
        bool mapEvidence, bool minimapEvidence, bool focused) {
        const bool showMap = focused && frameId != 0 && mapEvidence &&
            MapUiStateController::IsStableBigMap(stableState);
        const bool showMinimap = focused && frameId != 0 && minimapEvidence && !mapEvidence &&
            MapUiStateController::IsStableGameplay(stableState);
        if (showMap != frame_.mapVisible || showMinimap != frame_.minimapVisible) {
            // A late result from before a menu/transition cannot resurrect the
            // previous overlay when the same surface is seen again.
            frame_.visibleSinceFrame = frameId;
        }
        frame_.frameId = frameId;
        frame_.capturedAt = capturedAt;
        frame_.maximumAge = maximumAge;
        frame_.mapVisible = showMap;
        frame_.minimapVisible = showMinimap;
        return frame_;
    }

    void Reset() { frame_ = {}; }

private:
    OverlayVisibilityFrame frame_;
};
