#pragma once

#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>
#include <cmath>
#include <vector>

inline bool HasMapViewportSupport(const std::vector<cv::Point2f>& source,
    const cv::Mat& inliers, const cv::Size& cropSize) {
    if (cropSize.width <= 0 || cropSize.height <= 0 || inliers.total() != source.size()) return false;
    std::vector<cv::Point2f> support;
    for (std::size_t i = 0; i < source.size(); ++i)
        if (inliers.at<unsigned char>(static_cast<int>(i))) support.push_back(source[i]);
    if (support.size() < 12) return false;
    // A field-verified minimap reference may cover terrain entirely below the
    // viewport center. Quadrant occupancy is not a useful validity test for
    // that translation/zoom fit. Require two-dimensional spatial extent instead.
    const auto bounds = cv::boundingRect(support);
    std::vector<cv::Point2f> hull;
    cv::convexHull(support, hull);
    return bounds.width >= cropSize.width * 0.05 && bounds.height >= cropSize.height * 0.05 &&
        cv::contourArea(hull) >= static_cast<double>(cropSize.area()) * 0.002;
}

// The game map and marker projection are axis-aligned. A projective fit can
// explain a small cluster of matches while sending the crop corners far away.
// Use RANSAC to select terrain correspondences, then fit only zoom and pan.
inline cv::Mat FitMapViewportTransform(const std::vector<cv::Point2f>& source,
    const std::vector<cv::Point2f>& target, cv::Mat& inliers) {
    inliers.release();
    if (source.size() < 12 || source.size() != target.size()) return {};
    const auto affine = cv::estimateAffinePartial2D(source, target, inliers,
        cv::RANSAC, 3.0, 2000, 0.995, 10);
    if (affine.empty() || cv::countNonZero(inliers) < 12) return {};
    const double rotation = std::atan2(affine.at<double>(1, 0), affine.at<double>(0, 0));
    if (!std::isfinite(rotation) || std::abs(rotation) > 2.0 * CV_PI / 180.0) return {};

    cv::Point2d sourceMean{}, targetMean{};
    const int count = cv::countNonZero(inliers);
    for (std::size_t i = 0; i < source.size(); ++i) {
        if (!inliers.at<unsigned char>(static_cast<int>(i))) continue;
        sourceMean += cv::Point2d(source[i]);
        targetMean += cv::Point2d(target[i]);
    }
    sourceMean *= 1.0 / count;
    targetMean *= 1.0 / count;
    double numerator = 0.0, denominator = 0.0;
    for (std::size_t i = 0; i < source.size(); ++i) {
        if (!inliers.at<unsigned char>(static_cast<int>(i))) continue;
        const auto p = cv::Point2d(source[i]) - sourceMean;
        const auto q = cv::Point2d(target[i]) - targetMean;
        numerator += p.dot(q);
        denominator += p.dot(p);
    }
    if (denominator < 1.0) return {};
    const double scale = numerator / denominator;
    const auto translation = targetMean - sourceMean * scale;
    if (!std::isfinite(scale) || scale <= 0.0 ||
        !std::isfinite(translation.x) || !std::isfinite(translation.y)) return {};
    // Re-evaluate support after removing rotation; the returned mask must
    // describe the actual transform used to draw markers.
    for (std::size_t i = 0; i < source.size(); ++i) {
        inliers.at<unsigned char>(static_cast<int>(i)) =
            cv::norm(cv::Point2d(source[i]) * scale + translation - cv::Point2d(target[i])) <= 3.0;
    }
    return (cv::Mat_<double>(3, 3) << scale, 0.0, translation.x,
        0.0, scale, translation.y, 0.0, 0.0, 1.0);
}
