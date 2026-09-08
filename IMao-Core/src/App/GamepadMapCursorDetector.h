#pragma once
#include "MapUiVisualDetector.h"

struct GamepadMapCursorDetection {
    bool visible = false;
    cv::Point2d center{};
    double radius = 0.0;
    double confidence = 0.0; // Geometric/brightness support score, not calibrated probability.
};

class GamepadMapCursorDetector {
public:
    // Full BGR/BGRA game-client capture; RECT supplies its physical dimensions.
    // Result is relative to the capture origin, never the OS cursor or an
    // assumed screen center. Ambiguous, obscured, and non-controller UI fail.
    static GamepadMapCursorDetection Detect(const cv::Mat& snapshot, const RECT& clientRect);
};
