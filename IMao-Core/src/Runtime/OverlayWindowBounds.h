#pragma once
#include <Windows.h>

namespace OverlayWindowBounds {
inline bool GameClient(HWND game, RECT& physical) {
    RECT client{}; POINT origin{};
    if (!GetClientRect(game, &client) || !ClientToScreen(game, &origin) || client.right <= 0 || client.bottom <= 0) return false;
    physical = {origin.x, origin.y, origin.x + client.right, origin.y + client.bottom}; return true;
}
inline bool Synchronize(HWND overlay, const RECT& physical) {
    RECT actual{}, client{};
    if (!GetWindowRect(overlay, &actual)) return false;
    if (!EqualRect(&actual, &physical) && !SetWindowPos(overlay, HWND_TOPMOST, physical.left, physical.top,
        physical.right - physical.left, physical.bottom - physical.top, SWP_NOACTIVATE)) return false;
    // Success is actual geometry, not merely a successful request or a cache.
    return GetWindowRect(overlay, &actual) && EqualRect(&actual, &physical) && GetClientRect(overlay, &client) &&
        client.right == physical.right - physical.left && client.bottom == physical.bottom - physical.top;
}
}
