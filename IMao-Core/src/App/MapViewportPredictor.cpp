#include "MapViewportPredictor.h"
#include "MapViewportGeometry.h"

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

cv::Mat FitFrameMotion(const std::vector<cv::Point2f>& from, const std::vector<cv::Point2f>& to,
    int& accepted) {
    cv::Mat mask;
    const auto model = FitMapViewportTransform(from, to, mask);
    if (model.empty()) return {};
    accepted = cv::countNonZero(mask);
    if (accepted < kMinimumInliers || accepted < from.size() * 0.55) return {};
    std::vector<cv::Point2f> support, hull;
    for (size_t i = 0; i < from.size(); ++i) if (mask.at<unsigned char>(static_cast<int>(i))) support.push_back(from[i]);
    cv::convexHull(support, hull);
    const auto bounds = cv::boundingRect(support);
    if (bounds.width < kTrackingFrameSize / 4 || bounds.height < kTrackingFrameSize / 4 ||
        cv::contourArea(hull) < kTrackingFrameSize * kTrackingFrameSize * 0.03) return {};
    const double scale = model.at<double>(0, 0);
    if (scale < 0.70 || scale > 1.40) return {};
    return model(cv::Rect(0, 0, 3, 2)).clone();
}

// Small normalized patch searches provide subpixel tracking using the imgproc
// module already shipped with the application. No extra video DLL is needed.
bool TrackPatch(const cv::Mat& source, const cv::Mat& target, const cv::Point2f& point, cv::Point2f& found) {
    constexpr int patchSize = 15, searchSize = 39, searchRadius = (searchSize - patchSize) / 2;
    if (point.x < 20 || point.y < 20 || point.x >= source.cols - 20 || point.y >= source.rows - 20) return false;
    cv::Mat patch, search, scores;
    cv::getRectSubPix(source, cv::Size(patchSize, patchSize), point, patch);
    cv::getRectSubPix(target, cv::Size(searchSize, searchSize), point, search);
    cv::matchTemplate(search, patch, scores, cv::TM_CCOEFF_NORMED);
    double peak = 0.0;
    cv::Point position;
    cv::minMaxLoc(scores, nullptr, &peak, nullptr, &position);
    if (peak < 0.85 || position.x <= 0 || position.y <= 0 ||
        position.x >= scores.cols - 1 || position.y >= scores.rows - 1) return false;
    const auto refine = [](double left, double center, double right) {
        const double curvature = left - 2.0 * center + right;
        return curvature < -1e-6 ? std::clamp(0.5 * (left - right) / curvature, -0.5, 0.5) : 0.0;
    };
    const double dx = refine(scores.at<float>(position.y, position.x - 1), peak, scores.at<float>(position.y, position.x + 1));
    const double dy = refine(scores.at<float>(position.y - 1, position.x), peak, scores.at<float>(position.y + 1, position.x));
    found = point + cv::Point2f(static_cast<float>(position.x - searchRadius + dx),
        static_cast<float>(position.y - searchRadius + dy));
    // Correlation peaks alone have a systematic fractional-pixel bias. Refine
    // against image gradients so repeated slow pans do not accumulate that bias.
    cv::Mat reference, gradientX, gradientY;
    cv::getRectSubPix(source, cv::Size(patchSize, patchSize), point, reference, CV_32F);
    cv::Sobel(reference, gradientX, CV_32F, 1, 0, 3, 0.125);
    cv::Sobel(reference, gradientY, CV_32F, 0, 1, 3, 0.125);
    double xx = 0.0, xy = 0.0, yy = 0.0;
    for (int y = 1; y < patchSize - 1; ++y) for (int x = 1; x < patchSize - 1; ++x) {
        const double gx = gradientX.at<float>(y, x), gy = gradientY.at<float>(y, x);
        xx += gx * gx; xy += gx * gy; yy += gy * gy;
    }
    const double determinant = xx * yy - xy * xy;
    if (determinant < 1e-4) return false;
    const double referenceMean = cv::mean(reference)[0];
    for (int iteration = 0; iteration < 8; ++iteration) {
        cv::Mat aligned;
        cv::getRectSubPix(target, reference.size(), found, aligned, CV_32F);
        const double brightness = cv::mean(aligned)[0] - referenceMean;
        double ex = 0.0, ey = 0.0;
        for (int y = 1; y < patchSize - 1; ++y) for (int x = 1; x < patchSize - 1; ++x) {
            const double error = aligned.at<float>(y, x) - reference.at<float>(y, x) - brightness;
            ex += gradientX.at<float>(y, x) * error;
            ey += gradientY.at<float>(y, x) * error;
        }
        const cv::Point2f correction(static_cast<float>((yy * ex - xy * ey) / determinant),
            static_cast<float>((xx * ey - xy * ex) / determinant));
        if (cv::norm(correction) > 2.0) return false;
        found -= correction;
        if (cv::norm(correction) < 0.005) break;
    }
    if (cv::norm(found - point) > searchRadius) return false;
    return true;
}

cv::Mat TrackFrameMotion(const cv::Mat& previous, const cv::Mat& current, int& accepted) {
    std::vector<cv::Point2f> points, from, to;
    cv::goodFeaturesToTrack(previous, points, 160, 0.01, 8.0);
    if (points.size() >= kMinimumInliers) {
        for (const auto& point : points) {
            cv::Point2f forward, backward;
            if (!TrackPatch(previous, current, point, forward) ||
                !TrackPatch(current, previous, forward, backward) || cv::norm(point - backward) > 0.75) continue;
            from.push_back(point); to.push_back(forward);
        }
        auto model = FitFrameMotion(from, to, accepted);
        if (!model.empty()) return model;
    }
    // Larger drags/zoom steps can exceed the bounded patch search.
    // Retain descriptor matching as a guarded fallback for those frames.
    auto orb = cv::ORB::create(450, 1.2f, 6, 15, 0, 2, cv::ORB::HARRIS_SCORE, 21, 12);
    std::vector<cv::KeyPoint> previousKeys, currentKeys;
    cv::Mat previousDescriptors, currentDescriptors;
    orb->detectAndCompute(previous, cv::noArray(), previousKeys, previousDescriptors);
    orb->detectAndCompute(current, cv::noArray(), currentKeys, currentDescriptors);
    if (previousDescriptors.empty() || currentDescriptors.empty()) return {};
    cv::BFMatcher matcher(cv::NORM_HAMMING, false);
    std::vector<std::vector<cv::DMatch>> pairs;
    matcher.knnMatch(previousDescriptors, currentDescriptors, pairs, 2);
    from.clear(); to.clear();
    for (const auto& pair : pairs) {
        if (pair.size() < 2 || pair[0].distance >= 0.78f * pair[1].distance) continue;
        from.push_back(previousKeys[pair[0].queryIdx].pt);
        to.push_back(currentKeys[pair[0].trainIdx].pt);
    }
    return FitFrameMotion(from, to, accepted);
}
}

void MapViewportPredictor::Reset() {
    hasPrediction_ = false;
    currentFrameAnchored_ = false;
    revision_ = 0;
    trackingFailures_ = 0;
    prediction_ = {};
    referencePrediction_ = {};
    previousFrame_.release();
    latestFrame_.release();
}

void MapViewportPredictor::Confirm(int sceneId, const Coordinate& centerMapCoordinate,
    const std::vector<cv::Point2f>& captureCorners) {
    if (sceneId == 0 || !std::isfinite(centerMapCoordinate.x) || !std::isfinite(centerMapCoordinate.y) ||
        captureCorners.size() != 4 || !std::isfinite(SpanX(captureCorners)) || !std::isfinite(SpanY(captureCorners)) ||
        SpanX(captureCorners) < 1.0 || SpanY(captureCorners) < 1.0) {
        return;
    }
    prediction_.sceneId = sceneId;
    prediction_.centerMapCoordinate = centerMapCoordinate;
    prediction_.captureCorners = captureCorners;
    prediction_.isConfirmed = true;
    prediction_.isPredicted = false;
    prediction_.confidence = 3;
    prediction_.revision = revision_;
    hasPrediction_ = true;
    trackingFailures_ = 0;
    previousFrame_ = latestFrame_;
    referencePrediction_ = prediction_;
    currentFrameAnchored_ = !latestFrame_.empty();
}

bool MapViewportPredictor::SetDragCenter(const Coordinate& centerMapCoordinate) {
    if (!hasPrediction_) return false;
    if (std::hypot(prediction_.centerMapCoordinate.x - centerMapCoordinate.x,
        prediction_.centerMapCoordinate.y - centerMapCoordinate.y) >= 0.01) {
        ++revision_;
    }
    for (auto& corner : prediction_.captureCorners) {
        corner.x += static_cast<float>(centerMapCoordinate.x - prediction_.centerMapCoordinate.x);
        corner.y += static_cast<float>(centerMapCoordinate.y - prediction_.centerMapCoordinate.y);
    }
    prediction_.centerMapCoordinate = centerMapCoordinate;
    prediction_.isPredicted = true;
    prediction_.confidence = std::max(1, prediction_.confidence - 1);
    prediction_.revision = revision_;
    trackingFailures_ = 0;
    previousFrame_ = latestFrame_;
    referencePrediction_ = prediction_;
    currentFrameAnchored_ = false;
    return true;
}

bool MapViewportPredictor::GetPrediction(MapViewportPrediction& prediction) const {
    if (!hasPrediction_) return false;
    prediction = prediction_;
    return true;
}

bool MapViewportPredictor::ObserveFrame(const cv::Mat& mapCrop,
    MapViewportPrediction& prediction, int* inlierCount, double* scale) {
    currentFrameAnchored_ = false;
    if (inlierCount != nullptr) *inlierCount = 0;
    if (scale != nullptr) *scale = 1.0;
    const cv::Mat current = PrepareFrame(mapCrop);
    if (current.empty()) return false;
    latestFrame_ = current;
    // Track every map frame, including a pressed mouse button and inertia.
    // Earlier code skipped these exact frames and therefore only caught up
    // after the player released the map.
    if (previousFrame_.empty() || !hasPrediction_) {
        previousFrame_ = current;
        referencePrediction_ = prediction_;
        return false;
    }

    int accepted = 0;
    const cv::Mat affine = TrackFrameMotion(previousFrame_, current, accepted);
    if (affine.empty()) {
        // Keep the last geometrically anchored image across a bad capture.
        if (++trackingFailures_ >= 3) prediction_.confidence = std::max(0, prediction_.confidence - 1);
        return false;
    }
    const double estimatedScale = affine.at<double>(0, 0);

    cv::Mat inverse;
    cv::invertAffineTransform(affine, inverse);
    std::vector<cv::Point2f> screenCenter = { cv::Point2f(kTrackingFrameSize / 2.0f, kTrackingFrameSize / 2.0f) };
    cv::transform(screenCenter, screenCenter, inverse);
    const cv::Point2f previousCenter(kTrackingFrameSize / 2.0f, kTrackingFrameSize / 2.0f);
    const double mapPerScreenX = SpanX(referencePrediction_.captureCorners) / kTrackingFrameSize;
    const double mapPerScreenY = SpanY(referencePrediction_.captureCorners) / kTrackingFrameSize;
    if (!(mapPerScreenX > 0.0) || !(mapPerScreenY > 0.0)) return false;
	const double screenShift = cv::norm(screenCenter[0] - previousCenter);
    const Coordinate center(referencePrediction_.centerMapCoordinate.x + (screenCenter[0].x - previousCenter.x) * mapPerScreenX,
        referencePrediction_.centerMapCoordinate.y + (screenCenter[0].y - previousCenter.y) * mapPerScreenY);
    const double incrementalShift = std::hypot((center.x - prediction_.centerMapCoordinate.x) / mapPerScreenX,
        (center.y - prediction_.centerMapCoordinate.y) / mapPerScreenY);
    const double incrementalScale = SpanX(referencePrediction_.captureCorners) /
        (estimatedScale * SpanX(prediction_.captureCorners));
	const bool changed = incrementalShift >= 0.10 || std::abs(incrementalScale - 1.0) >= 0.0005;
	if (!changed) {
        // Keep the reference until its cumulative motion is applied. Advancing
        // it here used to discard every small pan/zoom step permanently.
		trackingFailures_ = 0;
		prediction_.confidence = std::min(5, prediction_.confidence + 1);
		prediction = prediction_;
		currentFrameAnchored_ = true;
		if (inlierCount != nullptr) *inlierCount = accepted;
		if (scale != nullptr) *scale = estimatedScale;
		return false;
	}

    const Coordinate oldCenter = referencePrediction_.centerMapCoordinate;
    prediction_.centerMapCoordinate = center;
    prediction_.captureCorners = referencePrediction_.captureCorners;
    for (auto& corner : prediction_.captureCorners) {
        corner.x = static_cast<float>(prediction_.centerMapCoordinate.x +
            (corner.x - oldCenter.x) / estimatedScale);
        corner.y = static_cast<float>(prediction_.centerMapCoordinate.y +
            (corner.y - oldCenter.y) / estimatedScale);
    }
    prediction_.isPredicted = true;
    trackingFailures_ = 0;
    prediction_.confidence = std::min(5, prediction_.confidence + 1);
    ++revision_;
    prediction_.revision = revision_;
    // Estimate against one anchored image over several frames instead of
    // integrating fractional resampling error at every capture. Rebase before
    // the bounded patch search runs out of overlap.
    if (screenShift >= 6.0 || std::abs(estimatedScale - 1.0) >= 0.025) {
        previousFrame_ = current;
        referencePrediction_ = prediction_;
    }
    if (inlierCount != nullptr) *inlierCount = accepted;
    if (scale != nullptr) *scale = estimatedScale;
    prediction = prediction_;
    currentFrameAnchored_ = true;
    return true;
}

bool MapViewportPredictor::TryBridgePrediction(const cv::Mat& requestCrop, const cv::Mat& currentCrop,
    const MapViewportPrediction& requestPrediction, MapViewportPrediction& currentPrediction,
    int* inlierCount, double* scale) {
    currentPrediction = {};
    if (inlierCount != nullptr) *inlierCount = 0;
    if (scale != nullptr) *scale = 1.0;
    if (requestCrop.empty() || currentCrop.empty() || requestCrop.size() != currentCrop.size()) return false;
    MapViewportPredictor bridge;
    MapViewportPrediction ignored;
    bridge.ObserveFrame(requestCrop, ignored);
    bridge.Confirm(requestPrediction.sceneId, requestPrediction.centerMapCoordinate,
        requestPrediction.captureCorners);
    if (!bridge.IsCurrentFrameAnchored()) return false;
    bridge.ObserveFrame(currentCrop, ignored, inlierCount, scale);
    if (!bridge.IsCurrentFrameAnchored()) return false;
    return bridge.GetPrediction(currentPrediction);
}

bool MapViewportPredictor::CanConfirmAfterBridge(const cv::Mat& pendingCrop, const cv::Mat& currentCrop,
    const MapViewportPrediction& pendingPrediction, const MapViewportPrediction& currentPrediction,
    double centerTolerance, double scaleRatioTolerance) {
    // The motion revision and raw viewport center can both change between two
    // independent absolute fixes. Compare their geometry in the same image.
    if (pendingPrediction.sceneId <= 0 || pendingPrediction.sceneId != currentPrediction.sceneId ||
        currentPrediction.captureCorners.size() != 4) return false;
    MapViewportPrediction projected;
    if (!TryBridgePrediction(pendingCrop, currentCrop, pendingPrediction, projected)) return false;
    const double oldWidth = SpanX(projected.captureCorners), oldHeight = SpanY(projected.captureCorners);
    const double width = SpanX(currentPrediction.captureCorners), height = SpanY(currentPrediction.captureCorners);
    const double distance = std::hypot(projected.centerMapCoordinate.x - currentPrediction.centerMapCoordinate.x,
        projected.centerMapCoordinate.y - currentPrediction.centerMapCoordinate.y);
    return std::isfinite(distance) && distance <= centerTolerance && oldWidth > 0.0 && oldHeight > 0.0 &&
        width > 0.0 && height > 0.0 && std::abs(width / oldWidth - 1.0) <= scaleRatioTolerance &&
        std::abs(height / oldHeight - 1.0) <= scaleRatioTolerance;
}
