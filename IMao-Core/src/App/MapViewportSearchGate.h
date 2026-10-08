#pragma once

#include <cstdint>

// When the full-screen map may spend a second on a cold start.
//
// The answer to a full-screen search is only usable if the frame it was asked about can still be
// found in the frame in front of the player when it arrives. That bridge spans about 67 pixels of
// map, and the search takes about a second, so a drag that is still going when the answer lands has
// already thrown it away. Measured on the 2026-10-08 evening session: eight of eighteen global
// searches ended as reason=bridge-rejected, and the 16.5 seconds they cost in a 100-second session
// bought nothing - the retry a second later is what the player felt as the map catching up after
// they let go.
//
// So the expensive rung waits for the map to hold still. The cheap one does not: a 50 ms local
// search bridges fine mid-drag (2.4% of them discarded, against 44% of the full-screen ones), and it
// is the rung that keeps the markers on screen while the player moves.
namespace map_viewport_gate {

// A little over three map loop cycles (80 ms each): enough to tell a pause from the gaps inside a
// drag, short enough not to add a pause of its own to every search.
inline constexpr std::int64_t kSettleMs = 250;

// The other side of the trade. A player who pans for ten seconds without stopping still gets one
// attempt, because never answering would be worse than one discarded answer.
inline constexpr std::int64_t kDeferralLimitMs = 2500;

// `deferredMilliseconds` is how long the full-screen rung has already been held back, and is ignored
// once the map has settled.
inline bool AllowFullScreenSearch(bool mapSettled, std::int64_t deferredMilliseconds) {
    return mapSettled || deferredMilliseconds >= kDeferralLimitMs;
}

}  // namespace map_viewport_gate
