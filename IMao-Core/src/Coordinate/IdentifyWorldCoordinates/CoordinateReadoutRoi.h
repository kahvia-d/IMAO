#pragma once
#include "../HudLayout.h"
#include <string>

// The client rectangle the coordinate readout occupies, or an empty rectangle plus the reason the
// frame was refused. Shared by the recognizer and its tests so the sanity floor cannot drift away
// from what is verified.
//
// The readout is anchored to the left and bottom edges at the client's single HUD scale (HudLayout.h),
// which is why it is the one box the old width/height model nearly got right on a 16:10 client. The
// old code instead listed three 16:9 resolutions and refused every other client, which silently
// disabled the readout on a 2560x1600 or 3840x2160 game.
inline cv::Rect CoordinateReadoutRoi(const cv::Size& frame, std::string& rejection) {
    rejection.clear();
    if (frame.width <= 0 || frame.height <= 0) { rejection = "empty-frame"; return {}; }
    const cv::Rect region = hud::MapBox(hud::Layout::For(frame.width, frame.height), hud::kCoordinateReadout);
    // The recognizer resizes the crop to 48px tall, so a smaller crop would be upscaled noise rather
    // than a readout. This is a sanity floor, not a resolution list.
    if (region.width < 60 || region.height < 24) { rejection = "readout-too-small"; return {}; }
    // A capture can briefly lag a window resize; that frame is refused rather than cropped from.
    if (region.x < 0 || region.y < 0 || region.x + region.width > frame.width ||
        region.y + region.height > frame.height) { rejection = "readout-outside-frame"; return {}; }
    return region;
}
