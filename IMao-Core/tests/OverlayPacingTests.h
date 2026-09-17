#pragma once

#include "Runtime/OverlayPacing.h"
#include <string>

// The overlay thread owns the input hooks, so the policy that decides when a hook may exist and how
// fast the overlay may present is what keeps this tool from adding latency to the game.
inline void TestOverlayPacing(void (*check)(bool, const std::string&)) {
    using namespace std::chrono_literals;
    using Clock = OverlayPacing::Clock;
    const auto now = Clock::time_point{100s};

    check(!OverlayPacing::WantsMouseHook(false, now, now), "no map interaction never wants the mouse hook");
    check(!OverlayPacing::WantsMouseHook(true, Clock::time_point{}, now), "a map that published no region never wants the mouse hook");
    check(OverlayPacing::WantsMouseHook(true, now, now), "freshly published regions want the mouse hook");
    check(OverlayPacing::WantsMouseHook(true, now - OverlayPacing::kMouseHookGrace + 1ms, now),
        "a click still in flight after the map closes keeps the mouse hook installed");
    check(!OverlayPacing::WantsMouseHook(true, now - OverlayPacing::kMouseHookGrace, now),
        "the mouse hook is removed once the grace period expires");
    check(OverlayPacing::WantsMouseHook(false, Clock::time_point{}, now, true),
        "a drag this hook already owns keeps it installed after the map closes");

    check(OverlayPacing::CapturePeriod(true) == OverlayPacing::kCaptureActivePeriod, "an attached overlay captures at the overlay rate");
    check(OverlayPacing::CapturePeriod(false) == OverlayPacing::kCaptureIdlePeriod, "an idle overlay captures at the recognition cadence");
    check(OverlayPacing::CapturePeriod(false) > OverlayPacing::CapturePeriod(true), "an idle overlay captures less often than an attached one");
    check(OverlayPacing::CapturePeriod(true, true) > OverlayPacing::CapturePeriod(true, false),
        "a slow capture backs off before the game is asked for another frame");
    check(OverlayPacing::CapturePeriod(false, true) > OverlayPacing::CapturePeriod(false, false),
        "the back-off also applies while the overlay is idle");
    check(OverlayPacing::kFramePeriod >= std::chrono::microseconds(33333), "the overlay presents no faster than its content changes");
}
