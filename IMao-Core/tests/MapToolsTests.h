#pragma once
#include "Runtime/MapToolsBridge.h"
#include "Runtime/OverlayWindowBounds.h"

template<class Expect> void TestMapTools(Expect expect) {
    using namespace std::chrono_literals;
    using Bridge = MapToolsBridge;
    const auto now = Bridge::Clock::now();
    GamepadContextSnapshot::View context;
    context.running = context.observable = context.bigMap = true;
    context.session = 8; context.generation = 12; context.gameHwnd = 201; context.gameProcessId = 2;
    context.profileId = "profile"; context.sceneName = "world";
    Bridge::Identity host{101, 1, 10}, game{201, 2, 20};
    Bridge::Observation observed{host, game, host.hwnd, true, true};
    const RECT bounds{400, 900, 900, 1000};
    Bridge bridge;
    const auto registered = bridge.Register(context, host, game, game.hwnd);
    expect(registered.registered && registered.phase == "panel", "tools registration has no controller prerequisite");
    auto state = bridge.Validate(context, observed, now + 60s);
    expect(state.registered && !state.canvasReady, "mouse-only tools survive without any controller samples");
    auto hiddenHost = observed; hiddenHost.hostVisible = false; hiddenHost.foreground = game.hwnd;
    expect(bridge.Validate(context, hiddenHost, now).registered, "registration tolerates pre-activation hidden host");
    state = bridge.Update(registered.sessionId, "route", "box", 1, bounds, true, 0);
    bridge.PublishFrame(true, now);
    state = bridge.Validate(context, observed, now);
    expect(state.registered && state.canvasReady && state.phase == "canvas", "fresh map plus focused real window arms canvas");
    Bridge::Sample sample; sample.sequence = 1; sample.buttons = 0;
    bridge.Push(state.sessionId, sample, now);
    expect(bridge.Drain(state.sessionId).size() == 1, "focused canvas consumes one ordered snapshot");
    bridge.SetDrawing(state.sessionId, true);
    const auto oldInput = state.inputRevision;
    state = bridge.Update(state.sessionId, "route", "box", 2, {410,900,910,1000}, true, 0);
    expect(state.registered && !state.drawing && state.inputRevision > oldInput, "layout change cancels draw without destroying window");
    bridge.PublishFrame(true, now); bridge.Validate(context, observed, now);
    sample.sequence = 2; sample.buttons = 0x1000; bridge.Push(state.sessionId, sample, now);
    bridge.SetDrawing(state.sessionId, true);
    state = bridge.Update(state.sessionId, "filter", "pan", 2, bounds, true, 0);
    expect(state.phase == "panel" && !state.drawing && bridge.Drain(state.sessionId).empty(), "filter page clears held draw and old snapshots");
    state = bridge.Update(state.sessionId, "route", "lasso", 3, bounds, true, 0);
    bridge.PublishFrame(true, now); bridge.Validate(context, observed, now);
    bridge.SetDrawing(state.sessionId, true);
    auto thirdParty = observed; thirdParty.foreground = 999;
    state = bridge.Validate(context, thirdParty, now);
    expect(state.registered && !state.canvasReady && !state.drawing, "third-party foreground revokes canvas only");
    bridge.PublishFrame(true, now); bridge.Validate(context, observed, now);
    sample.sequence = 3; sample.connected = false; bridge.Push(state.sessionId, sample, now);
    state = bridge.Validate(context, observed, now);
    expect(state.registered && !state.drawing, "device disconnect does not close mouse tools");
    bridge.PublishFrame(true, now); bridge.Validate(context, observed, now);
    sample.sequence = 4; sample.connected = true; bridge.Push(state.sessionId, sample, now);
    bridge.SetDrawing(state.sessionId, true);
    bridge.PublishFrame(true, now + 400ms);
    state = bridge.Validate(context, observed, now + 400ms);
    expect(state.registered && !state.drawing, "controller watchdog cancels held draw without ending mouse window");
    const auto beforeFilter = state.inputRevision;
    bridge.InvalidateCanvas("filter changed");
    state = bridge.Validate(context, observed, now + 400ms);
    expect(state.registered && state.inputRevision > beforeFilter, "filter mutation immediately revokes frozen gesture input epoch");
    bridge.FinishCanvas(state.sessionId, "done");
    state = bridge.Update(state.sessionId, "route", "box", 4, {450,910,950,1010}, false, 0);
    expect(state.phase == "panel" && state.canvasTool == "pan" && state.resultRevision == 1 &&
        state.bounds.left == 450 && !state.interactive, "stale result updates bounds but cannot rearm finished draw");
    state = bridge.Update(state.sessionId, "route", "start", 5, bounds, true, 1);
    bridge.PublishFrame(true, now); bridge.Validate(context, observed, now);
    bridge.SetDrawing(state.sessionId, true);
    auto unknown = context; unknown.observable = unknown.bigMap = false; ++unknown.generation;
    state = bridge.Validate(unknown, observed, now);
    expect(state.registered && !state.canvasReady && !state.drawing, "unknown map keeps real UI but cancels draw");
    auto gameplay = context; gameplay.bigMap = false; gameplay.gameplay = true;
    state = bridge.Validate(gameplay, observed, now);
    expect(!state.registered && state.phase == "ended", "confirmed gameplay invalidates map window binding");
    state = bridge.Register(context, host, game, game.hwnd);
    auto changedProfile = context; changedProfile.profileId = "other";
    expect(!bridge.Validate(changedProfile, observed, now).registered, "profile change revokes window registration");
    state = bridge.Register(context, host, game, game.hwnd);
    auto changedIdentity = observed; ++changedIdentity.host.thread;
    expect(!bridge.Validate(context, changedIdentity, now).registered, "reused HWND with different thread is rejected");
    state = bridge.Register(context, host, game, game.hwnd);
    bridge.Unregister(state.sessionId - 1);
    expect(bridge.Validate(context, observed, now).registered, "old unregister cannot close new session");
    bridge.Unregister(state.sessionId); bridge.Unregister(state.sessionId);
    expect(!bridge.Validate(context, observed, now).registered, "unregister is idempotent");
    bool rejected = false;
    try { bridge.Register(context, host, game, 999); } catch (const std::invalid_argument&) { rejected = true; }
    expect(rejected, "opening cannot steal focus from another foreground app");
    Bridge waiting;
    waiting.Register(context, host, game, game.hwnd);
    expect(!waiting.Validate(context, hiddenHost, now + 2s).registered,
        "unactivated tools registration expires after bounded focus grace");
    Bridge mouseOnly;
    auto mouseState = mouseOnly.Register(context, host, game, game.hwnd);
    mouseOnly.Update(mouseState.sessionId, "route", "box", 1, bounds, true);
    mouseOnly.PublishFrame(true, now); mouseOnly.Validate(context, observed, now);
    Bridge::Sample idle; idle.sequence = 1;
    mouseOnly.Push(mouseState.sessionId, idle, now); mouseOnly.SetDrawing(mouseState.sessionId, true);
    idle.connected = false; idle.sequence = 2;
    mouseOnly.Push(mouseState.sessionId, idle, now + 10ms);
    mouseOnly.PublishFrame(true, now + 500ms);
    mouseState = mouseOnly.Validate(context, observed, now + 500ms);
    expect(mouseState.registered && mouseState.drawing, "idle controller disconnect and timeout preserve ongoing mouse gesture");
    idle.connected = true; idle.buttons = 0x1000; idle.sequence = 3;
    mouseOnly.Push(mouseState.sessionId, idle, now + 500ms);
    idle.connected = false; idle.sequence = 4;
    mouseOnly.Push(mouseState.sessionId, idle, now + 510ms);
    expect(!mouseOnly.Validate(context, observed, now + 510ms).drawing, "actual controller-owned gesture is cancelled on disconnect");

    // Real hidden HWNDs reproduce Start/Stop sizing without presenting windows
    // or disturbing focus. Production calls the exact same synchronization.
    const auto foregroundBefore = GetForegroundWindow();
    const auto instance = GetModuleHandleW(nullptr);
    HWND fakeGame = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"IMao geometry test game",
        WS_POPUP, 80, 40, 2560, 1440, nullptr, nullptr, instance, nullptr);
    expect(fakeGame != nullptr, "geometry fixture game created");
    if (fakeGame) {
        for (int restart = 0; restart < 5; ++restart) {
            HWND overlay = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"IMao geometry test overlay",
                WS_POPUP, 100, 100, 1280, 800, nullptr, nullptr, instance, nullptr);
            RECT expected{}, actual{};
            const bool aligned = overlay && OverlayWindowBounds::GameClient(fakeGame, expected) &&
                OverlayWindowBounds::Synchronize(overlay, expected) && GetWindowRect(overlay, &actual) && EqualRect(&expected, &actual);
            expect(aligned, "same-process restarted overlay uses game bounds, never old 1280x800 viewport");
            if (overlay) {
                SetWindowPos(overlay, nullptr, 100, 100, 640, 360, SWP_NOZORDER | SWP_NOACTIVATE);
                expect(OverlayWindowBounds::Synchronize(overlay, expected), "unexpected real HWND drift is corrected despite unchanged game bounds");
                DestroyWindow(overlay);
            }
        }
        DestroyWindow(fakeGame);
    }
    expect(GetForegroundWindow() == foregroundBefore, "hidden geometry regression never activates test windows");
}
