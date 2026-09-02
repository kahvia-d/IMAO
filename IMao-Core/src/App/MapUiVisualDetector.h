#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <opencv2/core.hpp>
#include <Windows.h>

struct MapCompassDetection {
    bool visible = false;
    int goldPixels = 0;
    int cropPixels = 0;
};

// The upper-left world-map compass is gold on the current game UI.  This
// detector uses its fixed 16:9 layout and colour signature, captured from the
// Black Shores map reference, rather than relying on a keyboard transition.
class MapUiVisualDetector {
public:
    static MapCompassDetection DetectBigMapCompass(const cv::Mat& snapshot, const RECT& clientRect);
};
