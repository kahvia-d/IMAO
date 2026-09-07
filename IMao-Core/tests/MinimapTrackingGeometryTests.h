#pragma once

#include "Coordinate/VisualLocalization/MinimapTrackingGeometry.h"
#include <opencv2/calib3d.hpp>
#include <iostream>
#include <limits>

template<class Check>
void TestMinimapTrackingGeometry(Check check) {
    constexpr double scale = 194.0 / 184.0;
    const cv::Size size(184, 184);
    const cv::Point2d center(92.0, 92.0);
    const auto projectCenter = [&](const cv::Mat& transform) {
        return cv::Point2d(transform.at<double>(0, 0) * center.x +
                transform.at<double>(0, 1) * center.y + transform.at<double>(0, 2),
            transform.at<double>(1, 0) * center.x + transform.at<double>(1, 1) * center.y +
                transform.at<double>(1, 2));
    };

    cv::RNG rng(72194184);
    double similaritySquaredError = 0.0, translationSquaredError = 0.0;
    bool allAccepted = true, fixedGeometry = true;
    // Camera overlays change which terrain features survive. Use successive
    // asymmetric annulus subsets with independent subpixel detector noise and
    // wrong descriptor matches, while the true center follows known motion.
    for (int frame = 0; frame < 80; ++frame) {
        const cv::Point2d truth(5000.0 + frame * 0.35, -2300.0 + frame * 0.12);
        std::vector<cv::Point2f> source, target;
        for (int i = 0; i < 20; ++i) {
            const double angle = 0.65 + i * 0.07 + std::sin(frame * 0.21) * 0.15;
            const double radius = 49.0 + (i % 4) * 8.0;
            const cv::Point2d terrain(std::cos(angle) * radius, std::sin(angle) * radius);
            source.emplace_back(static_cast<float>(center.x + terrain.x + rng.gaussian(0.55)),
                static_cast<float>(center.y + terrain.y + rng.gaussian(0.55)));
            target.emplace_back(static_cast<float>(truth.x + scale * terrain.x),
                static_cast<float>(truth.y + scale * terrain.y));
        }
        for (int i = 0; i < 5; ++i) {
            source.emplace_back(rng.uniform(20.0f, 165.0f), rng.uniform(20.0f, 165.0f));
            target.emplace_back(static_cast<float>(truth.x + 250 + i * 20),
                static_cast<float>(truth.y - 180 + i * 12));
        }
        cv::Mat mask;
        const cv::Mat similarity = cv::estimateAffinePartial2D(source, target, mask,
            cv::RANSAC, 3.0, 2000, 0.99, 10);
        const cv::Mat translation = FitMinimapTrackingTranslation(source, target, size, mask);
        if (similarity.empty() || translation.empty()) { allAccepted = false; continue; }
        const auto similarityError = projectCenter(similarity) - truth;
        const auto translationError = projectCenter(translation) - truth;
        similaritySquaredError += similarityError.dot(similarityError);
        translationSquaredError += translationError.dot(translationError);
        fixedGeometry = fixedGeometry && translation.at<double>(0, 0) == scale &&
            translation.at<double>(1, 1) == scale && translation.at<double>(0, 1) == 0.0 &&
            translation.at<double>(1, 0) == 0.0;
    }
    check(allAccepted && fixedGeometry,
        "minimap changing terrain support preserves exact north-up scale without temporal lag");
    check(translationSquaredError < similaritySquaredError * 0.35,
        "minimap fixed terrain geometry suppresses center noise from asymmetric feature subsets");
    std::cout << "Minimap geometry similarityCenterRms=" << std::sqrt(similaritySquaredError / 80.0)
        << " fixedCenterRms=" << std::sqrt(translationSquaredError / 80.0) << '\n';

    std::vector<cv::Point2f> source{{20, 20}, {164, 20}, {20, 164}, {164, 164},
        {92, 20}, {92, 164}, {20, 92}, {164, 92}};
    std::vector<cv::Point2f> target;
    for (const auto& point : source) target.emplace_back(
        static_cast<float>(1000.0 + scale * (point.x - 92.0)),
        static_cast<float>(2000.0 + scale * (point.y - 92.0)));
    cv::Mat mask(static_cast<int>(source.size()), 1, CV_8UC1, cv::Scalar(1));
    auto translation = FitMinimapTrackingTranslation(source, target, size, mask);
    const auto first = projectCenter(translation);
    for (auto& point : source) point -= cv::Point2f(0.25f, -0.125f);
    mask.setTo(1);
    translation = FitMinimapTrackingTranslation(source, target, size, mask);
    const auto moved = projectCenter(translation);
    check(cv::norm(moved - first - cv::Point2d(0.25 * scale, -0.125 * scale)) < 0.001,
        "minimap geometry retains immediate subpixel movement rather than applying a dead zone");

    constexpr double calibratedScale = 1.075;
    for (std::size_t i = 0; i < source.size(); ++i) target[i] = cv::Point2f(
        static_cast<float>(1000.0 + calibratedScale * (source[i].x - 92.0)),
        static_cast<float>(2000.0 + calibratedScale * (source[i].y - 92.0)));
    mask.setTo(1);
    translation = FitMinimapTrackingTranslation(source, target, size, mask, calibratedScale);
    check(!translation.empty() && translation.at<double>(0, 0) == calibratedScale &&
        cv::norm(projectCenter(translation) - cv::Point2d(1000.0, 2000.0)) < 0.001,
        "minimap translation retains a calibrated terrain scale instead of substituting the nominal scale");

    for (std::size_t i = 0; i < source.size(); ++i) {
        const double angle = 15.0 * CV_PI / 180.0;
        const double x = source[i].x - 92, y = source[i].y - 92;
        target[i] = cv::Point2f(static_cast<float>(1000 + scale * (std::cos(angle) * x - std::sin(angle) * y)),
            static_cast<float>(2000 + scale * (std::sin(angle) * x + std::cos(angle) * y)));
    }
    mask.setTo(1);
    check(FitMinimapTrackingTranslation(source, target, size, mask).empty(),
        "minimap fixed geometry rejects rotated terrain instead of reusing similarity inliers");
    mask.setTo(1);
    target.front().x = std::numeric_limits<float>::quiet_NaN();
    check(FitMinimapTrackingTranslation(source, target, size, mask).empty(),
        "minimap fixed geometry rejects nonfinite correspondences");
}
