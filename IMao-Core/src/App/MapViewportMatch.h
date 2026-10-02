#pragma once

// The decision half of a full-screen viewport match: given a query's features, a set of map features and
// the pairs that survived the ratio test, work out whether they agree on a position and what that position
// is. Kept apart from *how* the pairs were found so the shipped matcher and any alternative can be compared
// through exactly the same gate - a difference in outcome then belongs to the matching, not to the judging
// (2026-10-02).

#include "../Coordinate/CoordinateStruct.h"
#include "../Feature/Match/FeatureMatch.h"
#include "MapViewportGeometry.h"
#include "MapViewportLocalizer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace MapViewportMatch {

// Pairs of image points, one per surviving match, with the indices the caller's own arrays use.
inline void CollectPairs(const ImageFeatureData& cropFeatures, const ImageFeatureData& candidateFeatures,
    const std::vector<cv::DMatch>& goodMatches, std::vector<cv::Point2f>& cropPoints,
    std::vector<cv::Point2f>& mapPoints) {
    cropPoints.clear();
    mapPoints.clear();
    cropPoints.reserve(goodMatches.size());
    mapPoints.reserve(goodMatches.size());
    for (const auto& match : goodMatches) {
        if (match.queryIdx < 0 || match.trainIdx < 0 ||
            match.queryIdx >= static_cast<int>(cropFeatures.imgKeypoints.size()) ||
            match.trainIdx >= static_cast<int>(candidateFeatures.imgKeypoints.size())) continue;
        cropPoints.push_back(cropFeatures.imgKeypoints[match.queryIdx].pt);
        mapPoints.push_back(candidateFeatures.imgKeypoints[match.trainIdx].pt);
    }
}

// The gate itself: fit, count inliers, check the reprojection error and the support, then read the centre
// and the corners off the transform.
inline bool DecideMatch(const std::vector<cv::Point2f>& cropPoints, const std::vector<cv::Point2f>& mapPoints,
    const cv::Mat& crop, MapViewportLocalizationResult& result) {
    if (cropPoints.size() < 12) return false;
    cv::Mat inlierMask;
    const cv::Mat homography = FitMapViewportTransform(cropPoints, mapPoints, inlierMask);
    if (homography.empty() || inlierMask.empty()) return false;
    result.inlierCount = cv::countNonZero(inlierMask);
    result.inlierRatio = static_cast<double>(result.inlierCount) / cropPoints.size();
    if (result.inlierCount < 12 || result.inlierRatio < 0.35) return false;

    const cv::Point2f cropCenter(crop.cols / 2.0f, crop.rows / 2.0f);
    std::array<bool, 4> quadrants{};
    std::vector<cv::Point2f> projected;
    cv::perspectiveTransform(cropPoints, projected, homography);
    std::vector<double> errors;
    errors.reserve(static_cast<std::size_t>(result.inlierCount));
    for (int index = 0; index < inlierMask.rows; ++index) {
        if (inlierMask.at<std::uint8_t>(index) == 0) continue;
        const auto& source = cropPoints[static_cast<std::size_t>(index)];
        const auto& expected = mapPoints[static_cast<std::size_t>(index)];
        const auto& mapped = projected[static_cast<std::size_t>(index)];
        errors.push_back(cv::norm(mapped - expected));
        const int quadrant = (source.x >= cropCenter.x ? 1 : 0) + (source.y >= cropCenter.y ? 2 : 0);
        quadrants[quadrant] = true;
    }
    std::sort(errors.begin(), errors.end());
    result.medianReprojectionError = errors.empty() ? 0.0 : errors[errors.size() / 2];
    result.coveredQuadrants = static_cast<int>(std::count(quadrants.begin(), quadrants.end(), true));
    if (!HasMapViewportSupport(cropPoints, inlierMask, crop.size()) ||
        result.medianReprojectionError > 3.0) return false;

    std::vector<cv::Point2f> cropCorners = {
        { 0.0f, 0.0f }, { static_cast<float>(crop.cols), 0.0f },
        { static_cast<float>(crop.cols), static_cast<float>(crop.rows) },
        { 0.0f, static_cast<float>(crop.rows) }
    };
    std::vector<cv::Point2f> corners;
    cv::perspectiveTransform(cropCorners, corners, homography);
    if (corners.size() != 4) return false;
    Coordinate center((corners[0].x + corners[2].x) / 2.0,
        (corners[0].y + corners[2].y) / 2.0);
    const double width = cv::norm(corners[1] - corners[0]);
    const double height = cv::norm(corners[3] - corners[0]);
    if (!std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(width) ||
        !std::isfinite(height) || width < 50.0 || height < 50.0) {
        return false;
    }
    result.centerMapCoordinate = center;
    result.captureCorners = std::move(corners);
    return true;
}

// The shipped shape: query features against an owned candidate set, whose keypoint indices are match
// indices.
inline bool TryMatch(const ImageFeatureData& cropFeatures, const cv::Mat& crop,
    const ImageFeatureData& candidateFeatures, const std::vector<cv::DMatch>& goodMatches,
    MapViewportLocalizationResult& result) {
    if (cropFeatures.imgDescriptors.empty() || candidateFeatures.imgDescriptors.empty()) return false;
    result.goodMatchCount = static_cast<int>(goodMatches.size());
    if (result.goodMatchCount < 12) return false;
    std::vector<cv::Point2f> cropPoints, mapPoints;
    CollectPairs(cropFeatures, candidateFeatures, goodMatches, cropPoints, mapPoints);
    return DecideMatch(cropPoints, mapPoints, crop, result);
}

// The alternative shape: query features against the resource's own rows, where a match names a row of the
// row list and the map point has to be looked up through it.
inline bool TryMatchRows(const ImageFeatureData& cropFeatures, const cv::Mat& crop,
    const RuntimeFeatureResources& resources, const std::vector<std::uint32_t>& rows,
    const std::vector<cv::DMatch>& goodMatches, MapViewportLocalizationResult& result) {
    if (cropFeatures.imgDescriptors.empty() || rows.empty()) return false;
    result.goodMatchCount = static_cast<int>(goodMatches.size());
    if (result.goodMatchCount < 12) return false;
    std::vector<cv::Point2f> cropPoints, mapPoints;
    cropPoints.reserve(goodMatches.size());
    mapPoints.reserve(goodMatches.size());
    for (const auto& match : goodMatches) {
        if (match.queryIdx < 0 || match.trainIdx < 0 ||
            match.queryIdx >= static_cast<int>(cropFeatures.imgKeypoints.size()) ||
            match.trainIdx >= static_cast<int>(rows.size())) continue;
        const auto row = rows[static_cast<std::size_t>(match.trainIdx)];
        if (row >= resources.map.imgKeypoints.size()) continue;
        cropPoints.push_back(cropFeatures.imgKeypoints[match.queryIdx].pt);
        mapPoints.push_back(resources.map.imgKeypoints[row].pt);
    }
    return DecideMatch(cropPoints, mapPoints, crop, result);
}

}   // namespace MapViewportMatch
