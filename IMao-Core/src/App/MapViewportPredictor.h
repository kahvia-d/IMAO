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
    // ObserveFrame's return value reports motion, not validity: a verified
    // stationary frame is anchored even when ObserveFrame returns false.
    bool IsCurrentFrameAnchored() const { return currentFrameAnchored_; }
    static bool TryBridgePrediction(const cv::Mat& requestCrop, const cv::Mat& currentCrop,
        const MapViewportPrediction& requestPrediction, MapViewportPrediction& currentPrediction,
        int* inlierCount = nullptr, double* scale = nullptr);
    static bool CanConfirmAfterBridge(const cv::Mat& pendingCrop, const cv::Mat& currentCrop,
        const MapViewportPrediction& pendingPrediction, const MapViewportPrediction& currentPrediction,
        double centerTolerance, double scaleRatioTolerance);

private:
    bool hasPrediction_ = false;
    bool currentFrameAnchored_ = false;
    std::uint64_t revision_ = 0;
    int trackingFailures_ = 0;
    MapViewportPrediction prediction_;
    MapViewportPrediction referencePrediction_;
    cv::Mat previousFrame_;
    cv::Mat latestFrame_;
};
