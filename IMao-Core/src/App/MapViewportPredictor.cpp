#include "MapViewportPredictor.h"

#include <opencv2/calib3d.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kTrackingFrameSize = 256;
constexpr int kMinimumInliers = 12;

cv::Mat PrepareFrame(const cv::Mat& source) {
    if (source.empty()) return {};
    cv::Mat gray;
    if (source.channels() == 1) gray = source;
    else if (source.channels() == 4) cv::cvtColor(source, gray, cv::COLOR_BGRA2GRAY);
    else cv::cvtColor(source, gray, cv::COLOR_BGR2GRAY);
    cv::Mat output;
    cv::resize(gray, output, cv::Size(kTrackingFrameSize, kTrackingFrameSize), 0.0, 0.0, cv::INTER_AREA);
    return output;
}

double SpanX(const std::vector<cv::Point2f>& corners) {
    return corners.size() == 4 ? cv::norm(corners[1] - corners[0]) : 0.0;
}

double SpanY(const std::vector<cv::Point2f>& corners) {
    return corners.size() == 4 ? cv::norm(corners[3] - corners[0]) : 0.0;
}
}

void MapViewportPredictor::Reset() {
    hasPrediction_ = false;
    prediction_ = {};
    previousFrame_.release();
}

void MapViewportPredictor::Confirm(int sceneId, const Coordinate& centerMapCoordinate,
    const std::vector<cv::Point2f>& captureCorners) {
    if (sceneId == 0 || captureCorners.size() != 4 || SpanX(captureCorners) < 1.0 || SpanY(captureCorners) < 1.0) {
        return;
    }
    prediction_.sceneId = sceneId;
    prediction_.centerMapCoordinate = centerMapCoordinate;
    prediction_.captureCorners = captureCorners;
    prediction_.isConfirmed = true;
    prediction_.isPredicted = false;
    prediction_.confidence = 3;
    hasPrediction_ = true;
}

bool MapViewportPredictor::SetDragCenter(const Coordinate& centerMapCoordinate) {
    if (!hasPrediction_) return false;
    prediction_.centerMapCoordinate = centerMapCoordinate;
    prediction_.isPredicted = true;
    prediction_.confidence = std::max(1, prediction_.confidence - 1);
    return true;
}

bool MapViewportPredictor::GetPrediction(MapViewportPrediction& prediction) const {
    if (!hasPrediction_) return false;
    prediction = prediction_;
    return true;
}

bool MapViewportPredictor::ObserveFrame(const cv::Mat& mapCrop,
    MapViewportPrediction& prediction, int* inlierCount, double* scale) {
    if (inlierCount != nullptr) *inlierCount = 0;
    if (scale != nullptr) *scale = 1.0;
    const cv::Mat current = PrepareFrame(mapCrop);
    if (current.empty()) return false;
    // Track every map frame, including a pressed mouse button and inertia.
    // Earlier code skipped these exact frames and therefore only caught up
    // after the player released the map.
    if (previousFrame_.empty() || !hasPrediction_) {
        previousFrame_ = current;
        return false;
    }

    auto orb = cv::ORB::create(450, 1.2f, 6, 15, 0, 2, cv::ORB::HARRIS_SCORE, 21, 12);
    std::vector<cv::KeyPoint> previousKeypoints;
    std::vector<cv::KeyPoint> currentKeypoints;
    cv::Mat previousDescriptors;
    cv::Mat currentDescriptors;
    orb->detectAndCompute(previousFrame_, cv::noArray(), previousKeypoints, previousDescriptors);
    orb->detectAndCompute(current, cv::noArray(), currentKeypoints, currentDescriptors);
    if (previousDescriptors.empty() || currentDescriptors.empty()) {
        previousFrame_ = current;
        prediction_.confidence = std::max(0, prediction_.confidence - 1);
        return false;
    }

    cv::BFMatcher matcher(cv::NORM_HAMMING, false);
    std::vector<std::vector<cv::DMatch>> pairs;
    matcher.knnMatch(previousDescriptors, currentDescriptors, pairs, 2);
    std::vector<cv::Point2f> trackedPrevious;
    std::vector<cv::Point2f> trackedCurrent;
    for (const auto& pair : pairs) {
        if (pair.size() < 2 || pair[0].distance >= 0.78f * pair[1].distance) continue;
        trackedPrevious.push_back(previousKeypoints[pair[0].queryIdx].pt);
        trackedCurrent.push_back(currentKeypoints[pair[0].trainIdx].pt);
    }
    if (trackedPrevious.size() < kMinimumInliers) {
        previousFrame_ = current;
        prediction_.confidence = std::max(0, prediction_.confidence - 1);
        return false;
    }

    cv::Mat inlierMask;
    const cv::Mat affine = cv::estimateAffinePartial2D(trackedPrevious, trackedCurrent, inlierMask,
        cv::RANSAC, 2.0, 1000, 0.995, 10);
    previousFrame_ = current;
    if (affine.empty() || affine.rows != 2 || affine.cols != 3) {
        prediction_.confidence = std::max(0, prediction_.confidence - 1);
        return false;
    }
    const int accepted = cv::countNonZero(inlierMask);
    const double a = affine.at<double>(0, 0);
    const double c = affine.at<double>(1, 0);
    const double estimatedScale = std::hypot(a, c);
    const double rotationDegrees = std::atan2(c, a) * 180.0 / CV_PI;
    if (accepted < kMinimumInliers || !std::isfinite(estimatedScale) || estimatedScale < 0.70 ||
        estimatedScale > 1.40 || std::abs(rotationDegrees) > 6.0) {
        prediction_.confidence = std::max(0, prediction_.confidence - 1);
        return false;
    }

    cv::Mat inverse;
    cv::invertAffineTransform(affine, inverse);
    std::vector<cv::Point2f> screenCenter = { cv::Point2f(kTrackingFrameSize / 2.0f, kTrackingFrameSize / 2.0f) };
    cv::transform(screenCenter, screenCenter, inverse);
    const cv::Point2f previousCenter(kTrackingFrameSize / 2.0f, kTrackingFrameSize / 2.0f);
    const double mapPerScreenX = SpanX(prediction_.captureCorners) / kTrackingFrameSize;
    const double mapPerScreenY = SpanY(prediction_.captureCorners) / kTrackingFrameSize;
    if (!(mapPerScreenX > 0.0) || !(mapPerScreenY > 0.0)) return false;
	const double screenShift = cv::norm(screenCenter[0] - previousCenter);
	const bool changed = screenShift >= 0.5 || std::abs(estimatedScale - 1.0) >= 0.003;
	if (!changed) {
		prediction_.confidence = std::min(5, prediction_.confidence + 1);
		prediction = prediction_;
		if (inlierCount != nullptr) *inlierCount = accepted;
		if (scale != nullptr) *scale = estimatedScale;
		return false;
	}

    const Coordinate oldCenter = prediction_.centerMapCoordinate;
    prediction_.centerMapCoordinate.x += (screenCenter[0].x - previousCenter.x) * mapPerScreenX;
    prediction_.centerMapCoordinate.y += (screenCenter[0].y - previousCenter.y) * mapPerScreenY;
    for (auto& corner : prediction_.captureCorners) {
        corner.x = static_cast<float>(prediction_.centerMapCoordinate.x +
            (corner.x - oldCenter.x) / estimatedScale);
        corner.y = static_cast<float>(prediction_.centerMapCoordinate.y +
            (corner.y - oldCenter.y) / estimatedScale);
    }
    prediction_.isPredicted = true;
    prediction_.confidence = std::min(5, prediction_.confidence + 1);
    if (inlierCount != nullptr) *inlierCount = accepted;
    if (scale != nullptr) *scale = estimatedScale;
    prediction = prediction_;
    return true;
}
