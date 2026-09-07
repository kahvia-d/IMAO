#pragma once
#include "../Coordinate/CoordinateStruct.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

// Absolute projection of one captured frame, used to register rendered pixels.
struct OverlayMotionSample {
    using Clock = std::chrono::steady_clock;
    Coordinate center, screenCenter;
    double pixelsPerUnit = 0.0;
    int scene = 0;
    std::uint64_t generation = 0, frameId = 0;
    Clock::time_point capturedAt{};
    bool reliable = false;
    // Advances only on an independently verified absolute map fix.
    std::uint64_t absoluteRevision = 0;
};

struct OverlayScreenTransform {
    double scale = 1.0;
    Coordinate offset;
    Coordinate Apply(const Coordinate& point) const {
        return {point.x * scale + offset.x, point.y * scale + offset.y};
    }
    Coordinate Inverse(const Coordinate& point) const {
        return {(point.x - offset.x) / scale, (point.y - offset.y) / scale};
    }
};
