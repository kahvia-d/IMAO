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

// Any of the game's right-side panels drawn over the full-screen map: the region list, an item or
// teleport detail, the custom-marker editor. While one is open the player is reading a menu rather
// than the map, so what the overlay draws for the current view is in the way - and after a region
// switch it is about to be wrong as well. Their backgrounds differ (the region list is dark and
// translucent, the detail panels are near-white), so colour alone cannot be the rule.
//
// Two statistics, either of which is enough, both measured 2026-10-08 over eleven captures - six
// with a panel and five with none:
//
//   * the region list's button column, which is the fraction of hud::kRegionPanelButtons that is
//     neutral light grey: 69.2% with the list open against 0.1..17.8% without it. Its dark separator
//     count keeps a flat bright surface out, which is the one shape the fraction would misread.
//   * the panel's own left edge, which is the strongest vertical step in hud::kMapSidePanel as a
//     fraction of the box's height: 0.82..1.00 for a panel against 0.26..0.45 for open map. A menu
//     has a straight full-height edge and terrain does not, and unlike the colours this is the same
//     for every panel the game draws.
//
// The numbers travel with the verdict so a threshold that starts misjudging real frames can be
// corrected from the log rather than from a guess.
struct MapPanelDetection {
    bool visible = false;
    double neutralFraction = 0.0;
    // Rows of the button box that are mostly dark - the list's own background showing between its
    // plates.
    int darkSeparatorRows = 0;
    double verticalEdgeFraction = 0.0;
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
    static MapPanelDetection DetectMapPanel(const cv::Mat& snapshot, const RECT& clientRect);
    static MapCompassTemplateInfo CompassTemplateInfo();
};
