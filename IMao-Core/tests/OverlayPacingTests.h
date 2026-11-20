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
        OverlayPacing::CaptureFrameSource source;
        check(source.Publish(1) == 0, "the startup frame is not republished by the capture loop");
        check(source.Publish(2) == 2, "a frame that arrived after startup is published with its own id");
        check(source.Publish(2) == 0, "the same frame is never published twice");
        check(source.Publish(3) == 3, "the next frame is published");
        check(source.Publish(3) == 0, "and it is not published twice either");
    }
    {
        // A backend with no source sequence, such as PrintWindow, reports 0 for every successful call.
        // Each of those is a frame the loop has never published, so it must never be refused and must
        // never repeat an id: a consumer that sees an unchanged id reads no new frame and the overlay
        // then waits for a game frame that already arrived.
        OverlayPacing::CaptureFrameSource source;
        const auto first = source.Publish(0);
        const auto second = source.Publish(0);
        const auto third = source.Publish(0);
        check(first != 0, "a backend that reports no sequence still publishes its first frame");
        check(second != 0, "and its second frame");
        check(third != 0, "and every frame after that");
        check(first != second && second != third, "the loop numbers those frames so each id is new");
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

    {
        OverlayPacing::FocusHandoffHold handoff;
        using Foreground = OverlayPacing::FocusHandoffForeground;
        check(!handoff.ShouldHold(true, Foreground::Tools, true, true, now),
            "a focused tools window with a ready map renders normally");
        check(handoff.ShouldHold(true, Foreground::None, false, true, now + 50ms),
            "the last map surface survives the empty foreground during tools return");
        check(handoff.ShouldHold(false, Foreground::Game, false, true, now + 150ms),
            "the map stays visible while the game regains focus but its frame is still stale");
        check(!handoff.ShouldHold(false, Foreground::Game, true, true, now + 200ms),
            "a fresh game map frame ends the handoff hold");
        check(!handoff.ShouldHold(false, Foreground::Game, false, true, now + 210ms),
            "a completed handoff cannot start another hold without a new tools focus");
    }
    {
        OverlayPacing::FocusHandoffHold handoff;
        using Foreground = OverlayPacing::FocusHandoffForeground;
        handoff.ShouldHold(true, Foreground::Tools, true, true, now);
        check(!handoff.ShouldHold(true, Foreground::Other, false, true, now + 20ms),
            "switching to another application never preserves map markers");
        check(!handoff.ShouldHold(true, Foreground::None, false, true, now + 30ms),
            "a later empty foreground cannot revive a cancelled handoff");
        handoff.ShouldHold(true, Foreground::Tools, true, true, now + 40ms);
        check(!handoff.ShouldHold(false, Foreground::None, false, true,
            now + 40ms + OverlayPacing::kFocusHandoffHoldLimit),
            "a focus handoff is held only for the bounded interval");
        handoff.ShouldHold(true, Foreground::Tools, true, true, now + 400ms);
        check(!handoff.ShouldHold(true, Foreground::None, false, false, now + 410ms),
            "a blank previous surface is never held");
    }

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
    // 动态采集档位与按场景降频策略测试
    {
        const auto smooth = OverlayPacing::ResolveEffectivePacing({0, 20, 80, 80, 150});
        const auto balanced = OverlayPacing::ResolveEffectivePacing({1, 20, 80, 80, 150});
        check(smooth.bigMapCapturePeriod == std::chrono::microseconds(16667) &&
            balanced.bigMapCapturePeriod == std::chrono::microseconds(33333),
            "2K 及以下大地图采集应为流畅 60fps、均衡 30fps");
        check(OverlayPacing::ResolveBaseCapturePeriod(smooth, OverlayPacing::CaptureScenario::BigMap, 1440) ==
            std::chrono::microseconds(16667) &&
            OverlayPacing::ResolveBaseCapturePeriod(smooth, OverlayPacing::CaptureScenario::BigMap, 2160) ==
            std::chrono::microseconds(33333) &&
            OverlayPacing::ResolveBaseCapturePeriod(balanced, OverlayPacing::CaptureScenario::BigMap, 2160) ==
            std::chrono::microseconds(50000),
            "4K 分辨率下大地图采集应自动护航至流畅 30fps / 均衡 20fps");
        check(OverlayPacing::FramePeriod(false, true, std::chrono::microseconds(16667)) ==
            OverlayPacing::kRouteCanvasFramePeriod &&
            OverlayPacing::FramePeriod(false, true, std::chrono::microseconds(33333)) ==
            OverlayPacing::kFramePeriod &&
            OverlayPacing::FramePeriod(false, false, std::chrono::microseconds(16667)) ==
            OverlayPacing::kFramePeriod,
            "仅大地图打开且采集高于 30fps 时，叠加层渲染才同步提至 60fps");
        check(balanced.minimapVisualCapturePeriod == std::chrono::microseconds(50000) &&
            balanced.minMapUpdateCycleMs == 250 &&
            balanced.bigMapDragCalibrationInterval == 500ms &&
            balanced.minimapVisualCapturePeriod < balanced.minimapReadoutCapturePeriod,
            "均衡档位下小地图视觉应跑 20fps 高频像素粘合，SURF 校准 250ms，大地图拖动校准 500ms");
        check(smooth.minimapVisualCapturePeriod == std::chrono::microseconds(33333) &&
            smooth.minMapUpdateCycleMs == 200 &&
            smooth.bigMapDragCalibrationInterval == 400ms,
            "流畅档位下小地图视觉应跑 30fps 高频粘合 + 200ms SURF 校准，大地图拖动校准 400ms");
        check(balanced.minimapReadoutCapturePeriod == std::chrono::microseconds(150000) &&
            balanced.ocrSubmissionInterval == 150ms &&
            balanced.readoutVisualProbeInterval == 450ms,
            "均衡档位下 OCR 模式采集与提交均对齐 150ms，后台探针控制在 450ms 以保恢复窗口");

        const auto customFastCap = OverlayPacing::ResolveEffectivePacing({2, 30, 90, 110, 180});
        check(customFastCap.bigMapCapturePeriod == std::chrono::microseconds(33333) &&
            customFastCap.minimapVisualCapturePeriod == std::chrono::microseconds(33333) &&
            customFastCap.minimapReadoutCapturePeriod == std::chrono::microseconds(180000) &&
            customFastCap.mapUpdateCycleMs == 90 && customFastCap.minMapUpdateCycleMs == 110 &&
            customFastCap.ocrSubmissionInterval == 180ms &&
            customFastCap.readoutVisualProbeInterval <= 500ms,
            "自定义档位下大地图与小地图视觉按采集上限跑像素粘合，探针间隔严守 <= 500ms 安全线");

        const auto customSlowCap = OverlayPacing::ResolveEffectivePacing({2, 5, 80, 80, 100});
        check(customSlowCap.minimapVisualCapturePeriod == std::chrono::microseconds(200000) &&
            customSlowCap.minimapReadoutCapturePeriod == std::chrono::microseconds(200000),
            "自定义采集上限慢于识别间隔时，小地图视觉与 OCR 模式均不得突破采集上限");

        check(OverlayPacing::ResolveScenarioCapturePeriod(balanced, OverlayPacing::CaptureScenario::BigMap, true) >
            OverlayPacing::ResolveScenarioCapturePeriod(balanced, OverlayPacing::CaptureScenario::BigMap, false),
            "动态场景采集周期同样遵循慢帧退避");
        check(OverlayPacing::CaptureFrameMaxAge(std::chrono::microseconds(33333)) == 250ms,
            "高频采集帧有效期保持 250ms 下限");
        check(OverlayPacing::CaptureFrameMaxAge(std::chrono::microseconds(200000)) == 600ms,
            "低频 5fps 采集帧有效期自动放宽至 3 倍周期以防误判过期");
    }
    {
        // Releasing on the frame that starts needing markers again is what keeps them off a held surface.
        OverlayPacing::HoldPresentPolicy policy;
        check(policy.ShouldHold(false), "a status-bar-only frame starts holding");
        for (int frame = 1; frame < OverlayPacing::kHeldFrameInterval; ++frame) policy.ShouldHold(false);
        check(!policy.ShouldHold(true), "the frame that needs markers again is released, not held");
    }
}
