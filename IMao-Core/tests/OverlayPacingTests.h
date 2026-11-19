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
        const auto low = OverlayPacing::ResolveEffectivePacing({2, 20, 80, 80, 150});
        check(smooth.bigMapCapturePeriod < balanced.bigMapCapturePeriod &&
            balanced.bigMapCapturePeriod < low.bigMapCapturePeriod,
            "大地图采集周期应按流畅 < 均衡 < 低占用递增");
        check(balanced.bigMapCapturePeriod < balanced.minimapVisualCapturePeriod &&
            balanced.minimapVisualCapturePeriod == std::chrono::microseconds(100000) &&
            balanced.minimapVisualCapturePeriod < balanced.minimapReadoutCapturePeriod,
            "均衡档位下小地图视觉应 1:1 对齐 100ms，介于大地图与 OCR 读数模式之间");
        check(smooth.minimapVisualCapturePeriod == std::chrono::microseconds(80000) &&
            low.minimapVisualCapturePeriod == std::chrono::microseconds(160000),
            "流畅与低占用档位下小地图视觉采集均 1:1 对齐各自小地图识别间隔");
        check(balanced.minimapReadoutCapturePeriod == std::chrono::microseconds(150000) &&
            balanced.ocrSubmissionInterval == 150ms &&
            balanced.readoutVisualProbeInterval == 600ms,
            "均衡档位下 OCR 模式采集与提交均对齐 150ms，后台探针放宽至 600ms");
        check(low.minimapReadoutCapturePeriod == std::chrono::microseconds(200000) &&
            low.readoutVisualProbeInterval == 1000ms,
            "低占用档位下 OCR 模式采集对齐 200ms，后台探针放宽至 1000ms");

        const auto customFastCap = OverlayPacing::ResolveEffectivePacing({3, 30, 90, 110, 180});
        check(customFastCap.bigMapCapturePeriod == std::chrono::microseconds(33333) &&
            customFastCap.minimapVisualCapturePeriod == std::chrono::microseconds(110000) &&
            customFastCap.minimapReadoutCapturePeriod == std::chrono::microseconds(180000) &&
            customFastCap.mapUpdateCycleMs == 90 && customFastCap.minMapUpdateCycleMs == 110 &&
            customFastCap.ocrSubmissionInterval == 180ms,
            "自定义档位下小地图视觉与 OCR 模式均自动向下对齐各自识别间隔");

        const auto customSlowCap = OverlayPacing::ResolveEffectivePacing({3, 5, 80, 80, 100});
        check(customSlowCap.minimapVisualCapturePeriod == std::chrono::microseconds(200000) &&
            customSlowCap.minimapReadoutCapturePeriod == std::chrono::microseconds(200000),
            "自定义采集上限慢于识别间隔时，小地图视觉与 OCR 模式均不得突破采集上限");

        // 纯绘制层小地图标记平滑过渡测试
        OverlayPacing::MinimapVisualSmoother smoother;
        const auto s0 = smoother.Update(true, 9, 1, 100.0, 200.0, 120.0, 120.0, 1.0, now);
        check(s0.x == 0.0 && s0.y == 0.0, "首次采样无残余偏移");
        // 玩家坐标向前移动 10 像素，首次出现反向平滑补偿 10 px
        const auto s1 = smoother.Update(true, 9, 1, 110.0, 200.0, 120.0, 120.0, 1.0, now + 33ms);
        check(s1.x > 9.9 && s1.x < 10.1 && s1.y == 0.0, "新坐标到达时应产生平滑衔接补偿");
        // 经过 65 ms 后残余偏移应平滑衰减到约 3.7 px
        const auto s2 = smoother.Update(true, 9, 1, 110.0, 200.0, 120.0, 120.0, 1.0, now + 98ms);
        check(s2.x > 2.5 && s2.x < 5.0, "平滑偏移应按时间常数指数衰减");
        // 静止足够长时间后应彻底归零锁定，保证不触发多余 Present
        const auto s3 = smoother.Update(true, 9, 1, 110.0, 200.0, 120.0, 120.0, 1.0, now + 348ms);
        const auto s4 = smoother.Update(true, 9, 1, 110.0, 200.0, 120.0, 120.0, 1.0, now + 598ms);
        check(s4.x == 0.0 && s4.y == 0.0 && s3.x >= 0.0, "静止收敛后残余偏移必须归零");
        // 超过 60 px 的大跳变（传送）必须直接瞬移，不拖尾
        const auto sJump = smoother.Update(true, 9, 1, 210.0, 200.0, 120.0, 120.0, 1.0, now + 631ms);
        check(sJump.x == 0.0 && sJump.y == 0.0, "大跳变传送应立即瞬移对齐");
        // 关闭开关时直接归零透传
        const auto sOff = smoother.Update(false, 9, 1, 220.0, 200.0, 120.0, 120.0, 1.0, now + 664ms);
        check(sOff.x == 0.0 && sOff.y == 0.0, "关闭平滑开关时应直接返回零偏移");

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
