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

// A surface whose probes missed one capture is not a surface the player has stopped looking at.
//
// Measured on the 2026-09-28 player log: the HUD probes alternate on adjacent captured frames (frames
// 45947/45949/45952/45955 published minimap=1/0/1/0, two to three frames apart all evening), and
// revoking on the first miss turned that into 265 marker-visibility flips in one hour, with 86
// minimap episodes and 10 map episodes shorter than a second. Holding the surface through the gap is
// what the state machine beside this policy already does for the same reason.
//
// The hold is deliberately short and one-sided: the moment the capture carries evidence for the
// *other* surface, or focus is lost, or the stable state has moved on, the surface is revoked on that
// frame. So closing the map still hides its markers on the frames that show the minimap, and only the
// "this capture had nothing to say" case is ever held.
constexpr std::chrono::milliseconds kEvidenceHold{250};

class OverlayVisibilityPolicy {
public:
    OverlayVisibilityFrame Observe(std::uint64_t frameId,
        OverlayVisibilityFrame::Clock::time_point capturedAt,
        std::chrono::milliseconds maximumAge, MapUiState stableState,
        bool mapEvidence, bool minimapEvidence, bool focused) {
        const bool live = focused && frameId != 0;
        const bool mapUp = MapUiStateController::IsStableBigMap(stableState);
        const bool gameplayUp = MapUiStateController::IsStableGameplay(stableState);
        if (live && mapEvidence) lastMapEvidenceAt_ = capturedAt;
        if (live && minimapEvidence) lastMinimapEvidenceAt_ = capturedAt;
        const bool showMap = live && ((mapEvidence && mapUp) ||
            (frame_.mapVisible && mapUp && !minimapEvidence && Held(capturedAt, lastMapEvidenceAt_)));
        const bool showMinimap = live && ((minimapEvidence && !mapEvidence && gameplayUp) ||
            (frame_.minimapVisible && gameplayUp && !mapEvidence && Held(capturedAt, lastMinimapEvidenceAt_)));
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

    void Reset() { frame_ = {}; lastMapEvidenceAt_ = {}; lastMinimapEvidenceAt_ = {}; }

private:
    // Evidence timestamps are the capture's own, so a repeated or out-of-order observation cannot
    // extend the hold.
    static bool Held(OverlayVisibilityFrame::Clock::time_point now,
        OverlayVisibilityFrame::Clock::time_point lastEvidence) {
        return lastEvidence != OverlayVisibilityFrame::Clock::time_point{} && now >= lastEvidence &&
            now - lastEvidence <= kEvidenceHold;
    }

    OverlayVisibilityFrame frame_;
    OverlayVisibilityFrame::Clock::time_point lastMapEvidenceAt_{};
    OverlayVisibilityFrame::Clock::time_point lastMinimapEvidenceAt_{};
};
