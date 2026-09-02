#pragma once

#include "../Coordinate/CoordinateStruct.h"

#include <opencv2/core.hpp>

#include <cstdint>
#include <vector>

struct MapViewportPrediction {
    int sceneId = 0;
    Coordinate centerMapCoordinate;
    std::vector<cv::Point2f> captureCorners;
    bool isConfirmed = false;
    bool isPredicted = false;
    int confidence = 0;
    std::uint64_t revision = 0;
};

// Keeps a short-lived, image-only estimate of the currently visible map.
// Confirmed visual matches remain the authority; this only prevents marker
// flicker while the user drags, scrolls or waits for the next local match.
class MapViewportPredictor {
public:
    void Reset();
    void Confirm(int sceneId, const Coordinate& centerMapCoordinate,
        const std::vector<cv::Point2f>& captureCorners);
    bool SetDragCenter(const Coordinate& centerMapCoordinate);
    bool ObserveFrame(const cv::Mat& mapCrop,
        MapViewportPrediction& prediction, int* inlierCount = nullptr,
        double* scale = nullptr);
    bool GetPrediction(MapViewportPrediction& prediction) const;

private:
    bool hasPrediction_ = false;
    std::uint64_t revision_ = 0;
    int trackingFailures_ = 0;
    MapViewportPrediction prediction_;
    cv::Mat previousFrame_;
};
