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

struct MapControlDetection {
    bool visible = false;
    bool mouse = false;
    bool controller = false;
    int controllerTriggerAnchors = 0;
    bool controllerSlider = false;
};

// The upper-left world-map compass is gold on the current game UI.  This
// detector uses its fixed 16:9 layout and colour signature, captured from the
// Black Shores map reference, rather than relying on a keyboard transition.
class MapUiVisualDetector {
public:
    static MapCompassDetection DetectBigMapCompass(const cv::Mat& snapshot, const RECT& clientRect);
    // Independent UI evidence, usable before any player coordinate is known.
    static bool DetectBigMapControls(const cv::Mat& snapshot, const RECT& clientRect);
    static MapControlDetection DetectBigMapControlLayout(const cv::Mat& snapshot, const RECT& clientRect);
};
