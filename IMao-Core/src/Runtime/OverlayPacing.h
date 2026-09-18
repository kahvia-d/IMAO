#pragma once
#include <chrono>
#include <cstdint>

// Pacing and input-hook policy for the in-game overlay. These decisions are separated from the
// rendering code so they can be tested without a game window.
namespace OverlayPacing {
using Clock = std::chrono::steady_clock;

// Presenting a layered overlay window faster than the content changes only adds compositor pressure:
// the swap chain blocks until the compositor takes the frame, and that time is added to the latency of
// every input event the overlay thread also services. Marker positions change at the capture rate
// (about 30 Hz), so 30 Hz presentation keeps the same visible result with far less blocking.
inline constexpr std::chrono::microseconds kFramePeriod{33333};

// The mouse hook decides synchronously whether a click belongs to the overlay, so it must be installed
// while the map publishes clickable regions. Installed for the whole session it would instead route
// every mouse event of the game through this frame-paced thread. The grace period covers a click that
// is already in flight when the map closes.
inline constexpr std::chrono::milliseconds kMouseHookGrace{500};

inline bool WantsMouseHook(bool mapInteractive, Clock::time_point regionsPublishedAt, Clock::time_point now, bool dragging = false) {
    // A drag that this hook already swallowed must keep owning its release, even when the map closes
    // or loses focus in between, so the game never receives a mouse-up without a matching down.
    if (dragging) return true;
    return mapInteractive && regionsPublishedAt != Clock::time_point{} && now - regionsPublishedAt < kMouseHookGrace;
}

// Capturing the game window is synchronous work inside the game, so it only follows the overlay rate
// while an overlay is actually attached; otherwise it runs at the recognition cadence, which is what
// the localization loop needs.
inline constexpr std::chrono::microseconds kCaptureActivePeriod{33333};
inline constexpr std::chrono::microseconds kCaptureIdlePeriod{80000};

// The fastest rate this process ever consumes captured frames. Windows Graphics Capture otherwise
// delivers every frame the game presents - on a 120 Hz game that is 120 full GPU readbacks plus a
// 14.7 MB copy per second for pixels nobody reads - while no consumer here asks for more than
// kCaptureActivePeriod. Telling the capture session this interval removes that work at the source,
// and it is deliberately a whole multiple of the display refresh so the frames we do get stay evenly
// spaced instead of arriving in pairs (measured: pairs of 100-176 ms game frames two seconds apart).
// It must never be larger than kCaptureActivePeriod, or the promise made above goes unmet.
inline constexpr std::chrono::microseconds kCaptureMinUpdateInterval{33333};

// A capture that took far longer than usual means the game is struggling to produce the extra frame
// this tool asks for. Waiting longer before asking again keeps such a stall from repeating back to
// back (measured: 116-176 ms stalls arrived in pairs two seconds apart).
inline constexpr double kSlowCaptureMs = 50.0;
inline constexpr std::chrono::microseconds kSlowCaptureBackoff{30000};

inline std::chrono::microseconds CapturePeriod(bool overlayActive) {
    return overlayActive ? kCaptureActivePeriod : kCaptureIdlePeriod;
}

inline std::chrono::microseconds CapturePeriod(bool overlayActive, bool slowCapture) {
    return CapturePeriod(overlayActive) + (slowCapture ? kSlowCaptureBackoff : std::chrono::microseconds::zero());
}

// Startup reads one frame to prove the capture works, and that frame is already stale by the time the
// capture loop runs - it is not a frame the loop observed. The loop therefore takes the first sequence
// it sees as its baseline and needs a newer one before it publishes, instead of re-publishing startup
// pixels as if they were current.
struct CaptureSequenceFilter {
    bool baselineKnown = false;
    std::uint64_t baseline = 0;
    std::uint64_t lastPublished = 0;

    // Returns true when this sequence is a frame the loop should publish, and records it.
    bool Accept(std::uint64_t sequence) {
        if (!baselineKnown) { baselineKnown = true; baseline = sequence; }
        if (sequence == baseline || sequence == lastPublished) return false;
        lastPublished = sequence;
        return true;
    }
};

// The capture loop publishes a frame id that every consumer uses as "have I seen this frame". Two kinds
// of backend have to end up in that one id space:
//
//  - A backend that numbers its frames (Windows Graphics Capture) hands the loop the same number
//    App::Init already consumed, so the filter skips exactly that one.
//  - A backend that numbers nothing (PrintWindow, this path's BitBlt fallback) reports no sequence at
//    all, and every successful call is a frame the loop has never published. The loop numbers those
//    itself, and it must hand out a *new* id each time, because a consumer that sees an unchanged id
//    concludes there is no new frame and never looks at the pixels.
//
// Those two cases cannot share the filter: the loop's synthetic number is derived from the last id it
// published, so when the filter swallows the first frame that number never advances - the same id is
// offered again forever and the tool publishes nothing for the whole session. Keep the decision here so
// the test can cover the numbering itself instead of only the filter in isolation.
struct CaptureFrameSource {
    CaptureSequenceFilter filter;
    std::uint64_t lastPublished = 0;

    // sourceSequence is the backend's frame number, or 0 when the backend reports none. Returns the
    // frame id to publish, or 0 when this frame must not be published.
    std::uint64_t Publish(std::uint64_t sourceSequence) {
        if (sourceSequence == 0) return ++lastPublished;
        if (!filter.Accept(sourceSequence)) return 0;
        lastPublished = sourceSequence;
        return sourceSequence;
    }
};

// The overlay window covers the whole game screen, so every present makes the desktop compositor
// blend that whole screen again - including the game's own frames, which a visible topmost layered
// window keeps out of its direct flip path. A frame whose content did not change therefore costs the
// game GPU time for nothing, and is skipped: the compositor keeps showing the last surface.
inline bool ShouldPresentFrame(std::uint64_t contentHash, std::uint64_t lastPresentedHash,
    bool hasPresented, bool windowVisible) {
    if (!hasPresented || !windowVisible) return true;
    return contentHash != lastPresentedHash;
}

// Nothing drawn for this many overlay frames (one second at the overlay rate) means the tool has
// nothing to show at all: hiding the window lets the compositor ignore it completely until the next
// marker appears.
inline constexpr int kFramesBeforeHidingIdleOverlay = 30;

inline bool ShouldHideIdleOverlay(int consecutiveEmptyFrames) {
    return consecutiveEmptyFrames >= kFramesBeforeHidingIdleOverlay;
}

// Diagnostic only. A visible topmost layered window costs the game two different things: the desktop
// compositor has to keep it in the composition, and every present makes the compositor blend the
// whole screen again. Holding the presents lets a frame-rate comparison say which of the two a
// measurement is actually paying for, because a held overlay keeps its window in the composition
// while skipping the render, the present and everything leading up to them.
//
// Markers are never held: their screen position changes with the map, so a held overlay would show
// them in the wrong place. Only a frame whose content is the status bar alone is a candidate.
inline constexpr int kHeldFrameInterval = 90; // about three seconds at the overlay rate

struct HoldPresentPolicy {
    bool holding = false;
    int heldFrames = 0;
    /// needsMarkers: whether the frame about to be rendered has markers to draw at a tracked position.
    /// Those are never held - their screen position moves with the map, so a held overlay would show
    /// them in the wrong place, and a frame that stopped needing them has to reach the screen to clear
    /// them. Only a frame with nothing but the status bar is a candidate.
    bool ShouldHold(bool needsMarkers) {
        if (holding) {
            if (++heldFrames >= kHeldFrameInterval) { holding = false; heldFrames = 0; }
            return holding;
        }
        if (needsMarkers) return false;
        holding = true;
        heldFrames = 0; // Counted when the next frame asks to hold, so the interval is exact.
        return true;
    }
};
}
