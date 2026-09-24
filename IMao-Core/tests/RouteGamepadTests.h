#pragma once
#include "Runtime/RouteGamepadBridge.h"
#include "Runtime/RouteGamepadControls.h"
#include "Runtime/MapToolsBridge.h"
#include "Runtime/OverlayPacing.h"
#include "Runtime/RoutePointSelectionInput.h"
#include <string>

inline void TestRouteGamepad(void (*check)(bool, const std::string&)) {
    using namespace std::chrono_literals;
    check(MapToolsBridge::SupportsCanvasTool("point"), "map tools native canvas accepts the single-point tool");
    check(RouteMarkerMouseActionFor(true, false, "pan", "p:42") == RouteMarkerMouseAction::Capture &&
        RouteMarkerMouseActionFor(true, false, "pan", "g:42") == RouteMarkerMouseAction::Capture,
        "planning point and grouped-marker clicks own the mouse down so the game cannot receive them");
    check(RouteMarkerMouseActionFor(true, false, "point", "g:42") == RouteMarkerMouseAction::Capture &&
        RouteMarkerMouseActionFor(true, false, "pan", "p:42", false, true) == RouteMarkerMouseAction::Capture &&
        RouteMarkerMouseActionFor(true, false, "pan", "p:42", false, false) == RouteMarkerMouseAction::Forward &&
        RouteMarkerMouseActionFor(true, true, "pan", "p:42") == RouteMarkerMouseAction::Forward &&
        RouteMarkerMouseActionFor(true, false, "pan", "route:tool") == RouteMarkerMouseAction::Forward,
        "fresh route markers stay captured across focus loss while stale, right-click and toolbar input is forwarded");
    check(RoutePointSelectionKeepsCanvas(true, "point") && !RoutePointSelectionKeepsCanvas(true, "pan") &&
        !RoutePointSelectionKeepsCanvas(false, "point"),
        "mouse toggles in route point mode keep the selection canvas active");
    check(RouteMarkerClickDismissesTools(true, false, true) &&
        !RouteMarkerClickDismissesTools(true, true, true) &&
        !RouteMarkerClickDismissesTools(true, false, false),
        "only mouse marker clicks dismiss tools; repeated canvas gamepad selections keep the window open");
    check(RouteGamepadSelectionHasFocus(true, false, true) &&
        !RouteGamepadSelectionHasFocus(true, true, false) &&
        RouteGamepadSelectionHasFocus(false, true, false) &&
        !RouteGamepadSelectionHasFocus(false, false, true),
        "tools gamepad selections require tools focus while overlay gamepad selections require overlay focus");
    check(IsRoutePointHit("p:42") && !IsRoutePointHit("g:42"), "gamepad single select only accepts a concrete marker hit");
    check(OverlayPacing::FramePeriod(true) == std::chrono::microseconds(16667) &&
        OverlayPacing::FramePeriod(false) == OverlayPacing::kFramePeriod,
        "active route canvas renders at 60 Hz while idle overlay pacing remains unchanged");
    check(RouteSnapSelectionTarget("p:42") == "p:42" && RouteSnapSelectionTarget("g:42") == "p:42" &&
        RouteSnapSelectionTarget("route:tool") .empty(), "snap hits resolve markers and grouped-marker anchors only");
    check(RoutePointActionFor("p:42") == RoutePointAction::Toggle &&
        RoutePointActionFor("g:42") == RoutePointAction::Toggle &&
        RoutePointActionFor("route:tool") == RoutePointAction::Snap,
        "A toggles a marker hit and requests snapping only when the cursor misses");
    const std::vector<RouteSnapPoint> snapCandidates{{"p:far", 20, 10}, {"p:near", 4, 3}};
    const auto nearestSnap = NearestRouteSnapPoint(snapCandidates, 0, 0);
    check(nearestSnap && nearestSnap->selectionTarget == "p:near" && nearestSnap->x == 4 && nearestSnap->y == 3,
        "snap chooses the nearest visible selectable point region");
    const auto start = RouteGamepadBridge::Clock::time_point{10s};
    {
        RouteGamepadDisplayLease lease;
        const RouteGamepadDisplayLease::Identity host{101, 201, 301}, game{102, 202, 302};
        const RouteGamepadDisplayLease::Observation focused{host, game, host.hwnd, true, true};
        lease.Begin(1, "local", host, game); lease.End(1, start);
        check(!lease.Read(1, "local", focused, start + 10ms).visible,
            "unfocused prepared host never receives a return display lease");
        lease.Begin(2, "local", host, game);
        check(lease.ConfirmFocused(2, focused), "live matching foreground identities verify the input host");
        check(!lease.Read(2, "local", focused, start).visible, "active input does not display a return notice");
        lease.End(2, start);
        check(lease.Read(2, "local", focused, start + 10ms).visible &&
            !lease.ConfirmFocused(2, focused), "ended host retains pixels without regaining input focus authorization");
        check(lease.ReturnStatus(2, "local", focused, true, start + 100ms) &&
            lease.Read(2, "local", focused, start + 100ms).failed, "failed return exposes explicit B retry notice");
        lease.End(2, start + 14s);
        lease.ReturnStatus(2, "local", focused, true, start + 14s);
        check(!lease.Read(2, "local", focused, start + 15s).visible,
            "repeated End and failed status cannot extend the fifteen second display lease");
        check(!lease.ReturnStatus(2, "local", focused, false, start + 16s), "expired display cannot automatically revive");
        lease.Begin(3, "local", host, game); lease.ConfirmFocused(3, focused); lease.End(3, start);
        lease.ReturnStatus(3, "local", focused, true, start + 1s);
        lease.ReturnStatus(3, "local", focused, false, start + 10s);
        check(lease.Read(3, "local", focused, start + 20s).visible,
            "one explicit failed-to-returning retry starts a new bounded display interval");
        lease.ReturnStatus(3, "local", focused, false, start + 24s);
        check(!lease.Read(3, "local", focused, start + 25s).visible, "repeated returning status does not refresh retry interval");
        for (int mismatch = 0; mismatch < 5; ++mismatch) {
            lease.Begin(4, "local", host, game); lease.ConfirmFocused(4, focused); lease.End(4, start);
            auto changed = focused;
            if (mismatch == 0) changed.foreground = 999;
            if (mismatch == 1) changed.host.process++;
            if (mismatch == 2) changed.game.thread++;
            if (mismatch == 3) changed.hostVisible = false;
            if (mismatch == 4) changed.game = {};
            check(!lease.Read(4, "local", changed, start + 1s).visible &&
                !lease.Read(4, "local", focused, start + 2s).visible,
                "host destruction, AltTab or identity reuse permanently revoke that display lease");
        }
        lease.Begin(5, "local", host, game); lease.ConfirmFocused(5, focused); lease.End(5, start);
        check(!lease.Read(5, "other-profile", focused, start + 10ms).visible, "account change revokes return display");
    }
    RouteGamepadBridge bridge;
    bridge.PublishFrame(true, "local", 7, start);
    auto state = bridge.Prepare(123, 456, 789, "local", 7, start);
    check(!state.active && state.phase == "awaitingFocus", "route gamepad never activates before input-host focus is confirmed");
    RouteGamepadBridge::Sample sample; sample.sequence = 1;
    state = bridge.Push(state.sessionId, sample, false, start + 20ms);
    check(state.phase == "ended" && bridge.Drain(state.sessionId).empty(), "game foreground cannot receive planning actions");
    const auto originalReason = state.message;
    bridge.End(state.sessionId, "secondary missing-map consequence");
    check(bridge.Read(start + 25ms).message == originalReason, "repeated Clear/End preserves original exit reason");
    sample.sequence = 2;
    const auto stoppedInput = bridge.Push(state.sessionId, sample, true, start + 26ms);
    check(stoppedInput.phase == "ended" && !stoppedInput.active && bridge.Drain(state.sessionId).empty(),
        "ended input stays disabled even when its retained display host is still foreground");
    sample.sequence = 1;
    bridge.PublishFrame(true, "local", 7, start + 30ms);
    state = bridge.Prepare(123, 456, 789, "local", 7, start + 30ms);
    state = bridge.Push(state.sessionId, sample, true, start + 40ms);
    check(state.active && bridge.Drain(state.sessionId).size() == 1, "focused route host accepts one current immutable sample");
    state = bridge.Push(state.sessionId, sample, true, start + 50ms);
    check(!state.active && state.phase == "ended", "duplicate or reordered sequence cancels the route gamepad session");
    bridge.PublishFrame(true, "local", 7, start + 60ms);
    state = bridge.Prepare(123, 456, 789, "local", 7, start + 60ms);
    sample.sequence = 1; bridge.Push(state.sessionId, sample, true, start + 70ms);
    bridge.PublishFrame(true, "local", 8, start + 80ms);
    check(bridge.Read(start + 80ms).phase == "ended" && bridge.Drain(state.sessionId).empty(), "new map generation discards pending input without committing it");
    bridge.PublishFrame(true, "local", 8, start + 90ms);
    state = bridge.Prepare(123, 456, 789, "local", 8, start + 90ms);
    bridge.Push(state.sessionId, sample, true, start + 100ms);
    check(bridge.Read(start + 450ms).phase == "ended", "lost input stream expires even when the host remains focused");
    bool rejected = false;
    try { bridge.Prepare(123, 456, 789, "local", 8, start + 500ms); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "stale presented toolbar cannot start a route input session");

    constexpr unsigned A = 0x1000, B = 0x2000;
    RouteGamepadControls controls;
    bridge.PublishFrame(true, "local", 8, start + 600ms);
    state = bridge.Prepare(123, 456, 789, "local", 8, start + 600ms);
    sample.sequence = 1; sample.buttons = A;
    bridge.Push(state.sessionId, sample, true, start + 620ms);
    bridge.Update(state.sessionId, "cursor", "route:tool:box", "box", true, "");
    controls.Update(0, 0, 0, false, true, false, 600);
    controls.Update(A, 0, 0, false, true, false, 620);
    bridge.DiscardPendingInput(state.sessionId); controls.Reset();
    bridge.PublishFrame(true, "local", 8, start + 640ms);
    const auto preserved = bridge.Read(start + 640ms);
    check(preserved.active && preserved.sessionId == state.sessionId && !preserved.drawing &&
        bridge.Drain(state.sessionId).empty(), "toolbar layout change discards old input without ending focused host session");
    const auto oldRelease = controls.Update(0, 0, 0, false, true, false, 660);
    check(!oldRelease.activate && !oldRelease.commitDraw, "layout change cannot submit the old held A on release");
    controls.Update(B, 0, 0, false, true, false, 680);
    check(controls.Update(0, 0, 0, false, true, false, 700).exit, "B exits normally after a layout rebind reaches neutral");
    controls.Reset();
    check(!controls.Update(A, 0, 0, false, false, false, 0).activate, "held entry input must first be released");
    controls.Update(0, 0, 0, false, false, false, 20);
    check(controls.Update(8, 0, 0, false, false, false, 40).direction == 1, "D-pad navigates original toolbar buttons");
    controls.Update(0, 0, 0, false, false, false, 60);
    check(!controls.Update(A, 0, 0, false, false, false, 80).activate &&
        controls.Update(0, 0, 0, false, false, false, 100).activate, "toolbar A confirms on release exactly once");
    controls.Reset(); controls.Update(0, 0, 0, false, true, false, 0);
    check(controls.Update(A, 0, 0, false, true, false, 20).beginDraw, "cursor mode freezes its gesture on A down");
    const auto moving = controls.Update(A, 1, 1, false, true, false, 120);
    check(moving.dx > 0 && moving.dy < 0 && !moving.commitDraw, "left stick moves virtual cursor while A holds without submitting");
    controls.Update(A, 1, 0, false, true, false, 220);
    check(controls.Update(0, 0, 0, false, true, false, 320).commitDraw &&
        !controls.Update(0, 0, 0, false, true, false, 340).commitDraw, "long A release commits one drawing then disarms");
    controls.Reset(); controls.Update(0, 0, 0, false, true, false, 0);
    controls.Update(A, 0, 0, false, true, false, 20);
    auto release = controls.Update(0, 0, 0, false, true, false, 100);
    check(release.cancelDraw && !release.commitDraw, "short A tap cannot submit a box or lasso");
    controls.Reset(); controls.Update(0, 0, 0, false, true, false, 0);
    controls.Update(A, 0, 0, false, true, false, 20);
    release = controls.Update(0, 0, 0, false, true, false, 400);
    check(release.cancelDraw && !release.commitDraw, "missing release samples cannot commit after a sampling stall");
    controls.Reset(); controls.Update(0, 0, 0, false, true, false, 0);
    controls.Update(A, 0, 0, false, true, false, 20);
    check(controls.Update(B, 0, 0, false, true, false, 40).cancelDraw &&
        controls.Update(0, 0, 0, false, true, false, 60).exit, "B cancels drawing before returning game focus on release");
    controls.Reset(); controls.Update(0, 0, 0, false, true, false, 0, true);
    check(controls.Update(A, 0, 0, false, true, false, 20, true).togglePoint &&
        !controls.Update(A, 1, 0, false, true, false, 120, true).togglePoint,
        "single-point action triggers on A down and does not repeat while held");
    controls.Update(0, 0, 0, false, true, false, 220, true);
    check(controls.Update(A, 0, 0, false, true, false, 240, true).togglePoint,
        "single-point action rearms after A release for the next press");
    controls.Update(0, 0, 0, false, true, false, 260, true);
    controls.Update(A, 0, 0, false, true, false, 280, true);
    check(controls.Update(B, 0, 0, false, true, false, 300, true).cancelDraw &&
        !controls.Update(0, 0, 0, false, true, false, 320, true).togglePoint,
        "B cancels a pending single-point activation without toggling");
    controls.Reset(); controls.Update(0, 0, 0, false, true, false, 0, true);
    controls.Update(0x0040, 0, 0, false, true, false, 20, true);
    controls.Update(0, 0, 0, false, true, false, 40, true);
    check(controls.Update(A, 0, 0, false, true, false, 60, true).togglePoint,
        "A remains the single-point action after an inert LS click");
    controls.Reset(); controls.Update(0, 0, 0, false, true, false, 0, true);
    const auto deadzone = controls.Update(0, .35, 0, false, true, false, 20, true);
    const auto halfAxis = controls.Update(0, .675, 0, false, true, false, 40, true);
    check(std::abs(deadzone.dx) < 1e-9 && std::abs(halfAxis.dx - .01) < 1e-9,
        "cursor speed scales smoothly from the stick deadzone to full deflection");
    controls.Reset(); controls.Update(0, 0, 0, false, true, false, 0);
    controls.Update(A, 0, 0, false, true, false, 20);
    check(controls.Update(A, 0, 0, true, true, false, 40).cancelDraw,
        "mixed triggers or right-stick input cancels an active drawing");
}
