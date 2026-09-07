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
    // the old map during its debounce. Neither is permission to keep drawing.
    state.Update({false, false});
    visibility.Publish(policy.Observe(11, start + 100ms, 500ms, state.State(), false, false, true));
    check(state.State() == MapUiState::BigMap && !visibility.Read()->AllowsMap(10, start + 100ms),
        "first missing map observation revokes old markers without resetting localization state");

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
    check(state.State() == MapUiState::Gameplay && !visibility.Read()->AllowsMinimap(13, start + 400ms),
        "first missing minimap observation hides cached markers while preserving gameplay recovery state");
    state.Update({false, true});
    visibility.Publish(policy.Observe(15, start + 500ms, 500ms, state.State(), false, true, true));
    check(!visibility.Read()->AllowsMinimap(13, start + 500ms) &&
        !visibility.Read()->AllowsMinimap(14, start + 500ms) &&
        visibility.Read()->AllowsMinimap(15, start + 500ms),
        "HUD reappearance requires a new marker frame and rejects late pre-transition results");

    visibility.Publish(policy.Observe(16, start + 600ms, 500ms, state.State(), false, true, true));
    check(visibility.Read()->AllowsMinimap(15, start + 600ms),
        "continuous gameplay observations do not invalidate each previous localization frame");
    check(!visibility.Read()->AllowsMinimap(16, start + 1100ms),
        "capture stalls expire visibility even if no worker publishes another marker frame");

    visibility.Publish(policy.Observe(17, start + 700ms, 500ms, state.State(), true, true, true));
    check(!visibility.Read()->AllowsMinimap(17, start + 700ms),
        "confirmed map controls suppress an incidental gameplay icon hit before the stable state changes");
    visibility.Publish(policy.Observe(18, start + 800ms, 500ms, state.State(), false, true, false));
    check(!visibility.Read()->AllowsMinimap(18, start + 800ms), "unfocused game UI cannot show markers");
    policy.Reset();
    visibility.Publish({});
    check(!visibility.Read()->AllowsMap(18, start + 800ms) && !visibility.Read()->AllowsMinimap(18, start + 800ms),
        "capture errors and stopped sessions revoke both overlay surfaces");
    visibility.Publish(policy.Observe(19, start + 900ms, 500ms, state.State(), false, true, true));
    check(!visibility.Read()->AllowsMinimap(18, start + 900ms) && visibility.Read()->AllowsMinimap(19, start + 900ms),
        "new capture after focus loss restores gameplay while excluding the snapshot from before focus loss");
}
