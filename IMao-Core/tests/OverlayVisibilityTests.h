#pragma once

#include "App/OverlayVisibilityPolicy.h"
#include "Runtime/SnapshotChannel.h"
#include <string>

inline void TestOverlayVisibility(void (*check)(bool, const std::string&)) {
    using namespace std::chrono_literals;
    using Clock = OverlayVisibilityFrame::Clock;
    const auto start = Clock::time_point{10s};
    MapUiStateController state;
    OverlayVisibilityPolicy policy;
    SnapshotChannel<OverlayVisibilityFrame> visibility;

    state.Update({true, false});
    state.Update({true, false});
    visibility.Publish(policy.Observe(10, start, 500ms, state.State(), true, false, true));
    check(visibility.Read()->AllowsMap(10, start), "confirmed map permits its current marker frame");

    // A displayed frame can still be fresh, and the localization state retains
    // the old map during its debounce. Neither is permission to keep drawing
    // forever - but one capture that simply had nothing to say is not the map
    // closing either: the player log this rule was rewritten from published
    // minimap=1/0/1/0 on frames two to three apart, all evening long.
    state.Update({false, false});
    visibility.Publish(policy.Observe(11, start + 100ms, 500ms, state.State(), false, false, true));
    check(state.State() == MapUiState::BigMap && visibility.Read()->AllowsMap(10, start + 100ms),
        "one silent capture holds confirmed markers instead of blinking them off");

    // Evidence for the other surface is not silence: it revokes on that frame.
    state.Update({false, true});
    visibility.Publish(policy.Observe(12, start + 200ms, 500ms, state.State(), false, true, true));
    check(!visibility.Read()->AllowsMap(10, start + 200ms) && !visibility.Read()->AllowsMinimap(12, start + 200ms),
        "transition frame cannot show the old map or prematurely activate gameplay markers");
    state.Update({false, true});
    visibility.Publish(policy.Observe(13, start + 300ms, 500ms, state.State(), false, true, true));
    check(visibility.Read()->AllowsMinimap(13, start + 300ms) && !visibility.Read()->AllowsMap(13, start + 300ms),
        "confirmed gameplay enables only the minimap surface");

    state.Update({false, false});
    visibility.Publish(policy.Observe(14, start + 400ms, 500ms, state.State(), false, false, true));
    check(state.State() == MapUiState::Gameplay && visibility.Read()->AllowsMinimap(13, start + 400ms),
        "one silent capture holds cached minimap markers while preserving gameplay recovery state");
    state.Update({false, true});
    visibility.Publish(policy.Observe(15, start + 500ms, 500ms, state.State(), false, true, true));
    check(!visibility.Read()->AllowsMinimap(12, start + 500ms) &&
        visibility.Read()->AllowsMinimap(13, start + 500ms) &&
        visibility.Read()->AllowsMinimap(15, start + 500ms),
        "a held surface keeps its own frames and still rejects the pre-transition ones");

    visibility.Publish(policy.Observe(16, start + 600ms, 500ms, state.State(), false, true, true));
    check(visibility.Read()->AllowsMinimap(15, start + 600ms),
        "continuous gameplay observations do not invalidate each previous localization frame");
    check(!visibility.Read()->AllowsMinimap(16, start + 1100ms),
        "capture stalls expire visibility even if no worker publishes another marker frame");

    visibility.Publish(policy.Observe(17, start + 700ms, 500ms, state.State(), true, true, true));
    check(!visibility.Read()->AllowsMinimap(17, start + 700ms),
        "confirmed map controls suppress an incidental gameplay icon hit before the stable state changes");
    policy.Reset();
    visibility.Publish({});
    check(!visibility.Read()->AllowsMap(17, start + 700ms) && !visibility.Read()->AllowsMinimap(17, start + 700ms),
        "capture errors and stopped sessions revoke both overlay surfaces");
    visibility.Publish(policy.Observe(18, start + 800ms, 500ms, state.State(), false, true, true));
    check(!visibility.Read()->AllowsMinimap(17, start + 800ms) && visibility.Read()->AllowsMinimap(18, start + 800ms),
        "new capture after focus loss restores gameplay while excluding the snapshot from before focus loss");

    // Focus is a property of the desktop, not of the map. The 2026-09-28 player log has the marker
    // layer withdrawn on the *first* frame this machine reported `focused=0` - while the big map was
    // still positively detected on that very frame (rawCompass=1 rawControls=1, stableState=BigMap).
    // A foreground that flickers away and back is not the player closing anything.
    {
        MapUiStateController held;
        OverlayVisibilityPolicy focusPolicy;
        SnapshotChannel<OverlayVisibilityFrame> focusVisibility;
        held.Update({false, true});
        held.Update({false, true});
        focusVisibility.Publish(focusPolicy.Observe(30, start + 3000ms, 500ms, held.State(), false, true, true));
        check(focusVisibility.Read()->AllowsMinimap(30, start + 3000ms), "confirmed gameplay is visible while focused");
        focusVisibility.Publish(focusPolicy.Observe(31, start + 3100ms, 500ms, held.State(), false, true, false));
        check(focusVisibility.Read()->AllowsMinimap(30, start + 3100ms),
            "one frame of lost focus does not blink a held surface off");
        focusVisibility.Publish(focusPolicy.Observe(32, start + 3400ms, 500ms, held.State(), false, true, false));
        check(focusVisibility.Read()->AllowsMinimap(30, start + 3400ms),
            "the focus hold covers consecutive unfocused frames");
        // A supporting capture while unfocused must not restart the hold - the clock is the capture's.
        focusVisibility.Publish(focusPolicy.Observe(33, start + 3900ms, 500ms, held.State(), false, true, false));
        check(!focusVisibility.Read()->AllowsMinimap(33, start + 4200ms),
            "the focus hold is bounded, so an alt-tab still clears the overlay");
        // Focus returning restores the surface on that frame.
        focusVisibility.Publish(focusPolicy.Observe(34, start + 4000ms, 500ms, held.State(), false, true, true));
        check(focusVisibility.Read()->AllowsMinimap(34, start + 4000ms), "regaining focus restores the surface immediately");
        focusVisibility.Publish(focusPolicy.Observe(35, start + 4100ms, 500ms, held.State(), false, true, false));
        check(focusVisibility.Read()->AllowsMinimap(34, start + 4100ms),
            "the hold restarts from the frame focus was last seen on");
    }

    // The hold is bounded on purpose: the player may have closed the map while the probes were blind,
    // so a surface that stops being supported is revoked `kEvidenceHold` after the last observation
    // that supported it - not on the next observation, and not never.
    {
        MapUiStateController held;
        OverlayVisibilityPolicy holdPolicy;
        SnapshotChannel<OverlayVisibilityFrame> heldVisibility;
        held.Update({true, false});
        held.Update({true, false});
        heldVisibility.Publish(holdPolicy.Observe(20, start + 2000ms, 500ms, held.State(), true, false, true));
        check(heldVisibility.Read()->AllowsMap(20, start + 2000ms), "the bounded hold starts from a visible map");
        heldVisibility.Publish(holdPolicy.Observe(21, start + 2100ms, 500ms, held.State(), false, false, true));
        heldVisibility.Publish(holdPolicy.Observe(22, start + 2200ms, 500ms, held.State(), false, false, true));
        check(heldVisibility.Read()->AllowsMap(20, start + 2200ms),
            "the bounded hold covers consecutive silent captures");
        heldVisibility.Publish(holdPolicy.Observe(23, start + 2300ms, 500ms, held.State(), false, false, true));
        check(!heldVisibility.Read()->AllowsMap(20, start + 2300ms),
            "the bounded hold expires once the evidence is older than the grace");
        // Every new supported observation restarts the grace, and the clock is the capture's own.
        heldVisibility.Publish(holdPolicy.Observe(24, start + 2400ms, 500ms, held.State(), true, false, true));
        check(heldVisibility.Read()->AllowsMap(24, start + 2400ms), "a supporting capture restores the map");
        heldVisibility.Publish(holdPolicy.Observe(25, start + 2500ms, 500ms, held.State(), false, false, true));
        check(heldVisibility.Read()->AllowsMap(24, start + 2500ms), "the grace restarts from the newest evidence");
        heldVisibility.Publish(holdPolicy.Observe(26, start + 2700ms, 500ms, held.State(), false, false, true));
        check(!heldVisibility.Read()->AllowsMap(24, start + 2700ms),
            "the grace is measured from the capture that carried the evidence");
    }
}
