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
}
