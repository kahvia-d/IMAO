#pragma once
#include <chrono>

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

inline std::chrono::microseconds CapturePeriod(bool overlayActive) {
    return overlayActive ? kCaptureActivePeriod : kCaptureIdlePeriod;
}
}
