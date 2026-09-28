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

// Focus is a property of the desktop, not of the map: another window taking the foreground for a
// moment does not mean the player stopped looking at an open map. Measured on the same 2026-09-28
// player log, this machine reports `focused=0` for **half** the session (3495 s of 7200 s, longest
// run 651 s) while the game keeps rendering and the minimap keeps matching, and every one of those
// intervals revoked the marker layer on its first frame - 19 of the layer's 38 flips in one stretch.
// The probes were never the problem: at 18:31:34 and 18:32:29 the big map was positively detected
// (rawCompass=1 rawControls=1, stableState=BigMap) on the very frame the layer was withdrawn.
//
// Two different silences are involved and they deserve different limits:
//   * the map is still up and the foreground moved somewhere else - we want to keep showing it, but
//     not indefinitely, or an alt-tab would leave the overlay on top of another application. The
//     capture freshness window (500 ms) is what finally ends this, so the limit stays under it.
//   * focus was never seen at all (the tool's own window owns it, or the game window handle is not
//     the one Windows reports as foreground) - treating that as "not live" is the documented rule
//     above ("focus is lost"), so it keeps the same limit rather than holding forever.
constexpr std::chrono::milliseconds kFocusHold{400};
// One map frame is ~80 ms, so the window has to outlast several of them to be worth anything.
constexpr std::chrono::milliseconds kFocusHandoffHold{350};

class OverlayVisibilityPolicy {
public:
    OverlayVisibilityFrame Observe(std::uint64_t frameId,
        OverlayVisibilityFrame::Clock::time_point capturedAt,
        std::chrono::milliseconds maximumAge, MapUiState stableState,
        bool mapEvidence, bool minimapEvidence, bool focused) {
        if (focused) lastFocusedAt_ = capturedAt;
        // "The game is up and in front of the player" for this frame. A foreground that flickers away
        // and back is invisible here; a sustained switch is still a switch, and the freshness window
        // in AllowsMap/AllowsMinimap is what finally expires a surface nobody is watching any more.
        const bool live = Observed(capturedAt) && frameId != 0;
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

    void Reset() { frame_ = {}; lastMapEvidenceAt_ = {}; lastMinimapEvidenceAt_ = {}; lastFocusedAt_ = {}; }

    // Whether a frame captured now may keep drawing. The render thread asks the same question, so the
    // two cannot disagree about whether the player is watching.
    bool Observed(OverlayVisibilityFrame::Clock::time_point now) const {
        return Held(now, lastFocusedAt_,
            frame_.mapVisible ? kFocusHandoffHold : kFocusHold);
    }

private:
    // Evidence timestamps are the capture's own, so a repeated or out-of-order observation cannot
    // extend the hold.
    static bool Held(OverlayVisibilityFrame::Clock::time_point now,
        OverlayVisibilityFrame::Clock::time_point lastEvidence,
        std::chrono::milliseconds limit = kEvidenceHold) {
        return lastEvidence != OverlayVisibilityFrame::Clock::time_point{} && now >= lastEvidence &&
            now - lastEvidence <= limit;
    }

    OverlayVisibilityFrame frame_;
    OverlayVisibilityFrame::Clock::time_point lastMapEvidenceAt_{};
    OverlayVisibilityFrame::Clock::time_point lastMinimapEvidenceAt_{};
    OverlayVisibilityFrame::Clock::time_point lastFocusedAt_{};
};
