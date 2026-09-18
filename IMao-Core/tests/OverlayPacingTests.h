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
    // The capture session is rate limited at the source, so the interval has to be small enough for
    // the fastest consumer here; a larger one would only add latency to every marker. Nothing needs
    // the idle cadence to be honoured by the capture session: a frame that arrives sooner than the
    // consumer asks for it is simply left unread.
    check(OverlayPacing::kCaptureMinUpdateInterval <= OverlayPacing::kCaptureActivePeriod,
        "the capture rate limit never starves the attached-overlay cadence");
    check(OverlayPacing::kCaptureMinUpdateInterval > std::chrono::microseconds::zero(),
        "a zero capture interval would still ask the game for every presented frame");

    // The startup frame App::Init read is not a frame the capture loop observed.
    {
        OverlayPacing::CaptureSequenceFilter filter;
        check(!filter.Accept(1), "the startup frame is not republished by the capture loop");
        check(filter.Accept(2), "a frame that arrived after startup is published");
        check(!filter.Accept(2), "the same frame is never published twice");
        check(filter.Accept(3), "the next frame is published");
        check(!filter.Accept(3), "and it is not published twice either");
    }
    {
        // A backend with no source sequence, such as PrintWindow, still starts from its own baseline.
        OverlayPacing::CaptureSequenceFilter filter;
        check(!filter.Accept(7), "a sequence the loop starts on is its baseline, not a frame to publish");
        check(filter.Accept(8), "the next sequence is published");
    }

    check(OverlayPacing::ShouldPresentFrame(7, 7, true, true) == false,
        "an unchanged overlay frame is not presented again");
    check(OverlayPacing::ShouldPresentFrame(8, 7, true, true),
        "changed content is presented");
    check(OverlayPacing::ShouldPresentFrame(7, 7, false, true),
        "the first frame of a session is always presented");
    check(OverlayPacing::ShouldPresentFrame(7, 7, true, false),
        "a hidden overlay window is filled before it is shown again");

    check(!OverlayPacing::ShouldHideIdleOverlay(0), "a freshly drawn overlay is never hidden");
    check(!OverlayPacing::ShouldHideIdleOverlay(OverlayPacing::kFramesBeforeHidingIdleOverlay - 1),
        "a short gap between markers keeps the overlay window visible");
    check(OverlayPacing::ShouldHideIdleOverlay(OverlayPacing::kFramesBeforeHidingIdleOverlay),
        "an overlay with nothing to draw for a second is hidden from the compositor");

    // The hold diagnostic keeps the window in the composition while it stops presenting into it.
    {
        OverlayPacing::HoldPresentPolicy policy;
        check(!policy.ShouldHold(true), "a frame that needs markers is never held");
        check(policy.ShouldHold(false), "a status-bar-only frame may be held");
        for (int frame = 1; frame < OverlayPacing::kHeldFrameInterval; ++frame)
            check(policy.ShouldHold(false), "a held frame keeps holding until the interval elapses");
        check(!policy.ShouldHold(false), "the hold is released once the interval elapses");
        check(policy.ShouldHold(false), "and the next status-bar-only frame starts a new hold");
    }
    {
        // Releasing on the frame that starts needing markers again is what keeps them off a held surface.
        OverlayPacing::HoldPresentPolicy policy;
        check(policy.ShouldHold(false), "a status-bar-only frame starts holding");
        for (int frame = 1; frame < OverlayPacing::kHeldFrameInterval; ++frame) policy.ShouldHold(false);
        check(!policy.ShouldHold(true), "the frame that needs markers again is released, not held");
    }
}
