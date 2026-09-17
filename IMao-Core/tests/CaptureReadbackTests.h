#pragma once

#include "Runtime/CaptureReadback.h"
#include <string>

// Windows Graphics Capture readback: what the frame callback does while the staging copy is still
// running. Answering this wrongly either drops every frame of the session (the client then reports a
// failed start) or stalls the callback on the GPU.
inline void TestCaptureReadback(void (*check)(bool, const std::string&)) {
    check(!CaptureReadback::MustWaitForCopy(S_OK, false), "a completed readback is used as soon as it maps");
    check(!CaptureReadback::MustWaitForCopy(S_OK, true), "a completed readback is used once frames are flowing");
    check(CaptureReadback::MustWaitForCopy(DXGI_ERROR_WAS_STILL_DRAWING, false),
        "the first frame waits for its copy instead of reporting a failed start");
    check(!CaptureReadback::MustWaitForCopy(DXGI_ERROR_WAS_STILL_DRAWING, true),
        "a pending copy is skipped once the previous frame can be shown");
    check(CaptureReadback::MustWaitForCopy(E_INVALIDARG, false),
        "a driver that rejects the non-blocking flag still produces a first frame");
    check(CaptureReadback::MustWaitForCopy(E_INVALIDARG, true),
        "a rejected non-blocking flag keeps the capture working after the first frame");
}
