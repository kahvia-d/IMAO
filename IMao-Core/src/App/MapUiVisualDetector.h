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

// The region-switch panel whose button column covers the right of the full-screen map. While it is
// open the player is picking another region rather than reading this one, so what the overlay draws
// for the current region is both in the way and about to be wrong.
//
// One statistic decides it: the fraction of the button box that is neutral light grey. Measured on
// the 2026-10-08 capture with the panel open against six captures without it: 69.2% against 0.1,
// 0.3, 3.5, 4.3, 6.1 and 11.3 percent. The box also carries the number so a threshold that starts
// misjudging real frames can be corrected from the log rather than from a guess.
struct MapRegionPanelDetection {
    bool visible = false;
    double neutralFraction = 0.0;
    // Rows of the box that are mostly dark - the panel's own background showing between its button
    // plates. Two or more of them is what separates a menu from a flat bright surface, which is the
    // one shape the colour fraction alone would misread.
    int darkSeparatorRows = 0;
    int sampledPixels = 0;
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
    static MapRegionPanelDetection DetectRegionPanel(const cv::Mat& snapshot, const RECT& clientRect);
    static MapCompassTemplateInfo CompassTemplateInfo();
};
