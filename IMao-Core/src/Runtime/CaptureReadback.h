#pragma once
#include <d3d11.h>

// Readback policy for the Windows Graphics Capture path. A captured frame arrives as a GPU texture and
// is copied into a CPU-visible staging texture, so the copy is asynchronous and the frame callback has
// to decide what to do while it is still running: show the previous frame, or wait for this one.
namespace CaptureReadback {

// The immediate context submits its work lazily and neither the frame pool nor this path presents, so a
// non-blocking Map can answer DXGI_ERROR_WAS_STILL_DRAWING for a frame that is already complete, and
// nothing in this path ever changes that answer. Every frame of the session was dropped this way, which
// the client reported as a core fault with no first frame.
//
// While no frame has been published the readback is not optional: the session has nothing to show and
// the failure reaches the player as a failed start, so the copy is awaited. Once frames flow, a copy
// that is still running is skipped instead, because stalling the frame callback on the GPU only delays
// the next arrival. A driver that rejects the non-blocking flag is treated like a pending copy, since
// dropping every frame is never the better answer.
inline bool MustWaitForCopy(HRESULT mapResult, bool hasPublishedFrame) {
    if (SUCCEEDED(mapResult)) return false;
    return !(mapResult == DXGI_ERROR_WAS_STILL_DRAWING && hasPublishedFrame);
}
}
