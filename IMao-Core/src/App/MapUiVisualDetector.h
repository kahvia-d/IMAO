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
    // Three percent gold in the box only says "something gold is here": the minimap sits underneath the
    // same box and a panel can paint its own emblem on it.  The widget itself is decided by the embedded
    // class-map template, and these are the numbers it scored (0..1).  `templateAvailable` is false only
    // if the compiled-in reference failed to decode, in which case the colour verdict stands alone.
    bool templateAvailable = false;
    bool templateVerified = false;
    double templateAgreement = 0.0;
    double templateCoverage = 0.0;
};

struct MapControlDetection {
    bool visible = false;
    bool mouse = false;
    bool controller = false;
    int controllerTriggerAnchors = 0;
    bool controllerSlider = false;
};

// The compiled-in compass reference.  Reported so a damaged embed fails loudly in the test suite instead
// of silently turning the probe into "never a compass".
struct MapCompassTemplateInfo {
    bool available = false;
    int width = 0;
    int height = 0;
    int maskPixels = 0;
    int decodedBytes = 0;
    const char* reason = "unloaded";  // ok | base64-empty | size-mismatch | mask-mismatch
};

// The upper-left world-map compass is gold on the current game UI.  This
// detector uses the reference HUD layout and colour signature, captured from the
// Black Shores map reference, rather than relying on a keyboard transition. The
// boxes are placed through HudLayout.h, so a non-16:9 client keeps them on the
// pixels the game actually draws them at.
class MapUiVisualDetector {
public:
    static MapCompassDetection DetectBigMapCompass(const cv::Mat& snapshot, const RECT& clientRect);
    // Independent UI evidence, usable before any player coordinate is known.
    static bool DetectBigMapControls(const cv::Mat& snapshot, const RECT& clientRect);
    static MapControlDetection DetectBigMapControlLayout(const cv::Mat& snapshot, const RECT& clientRect);
    static MapCompassTemplateInfo CompassTemplateInfo();
};
