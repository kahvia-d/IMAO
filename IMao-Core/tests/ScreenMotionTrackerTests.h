#pragma once

#include "App/ScreenMotionTracker.h"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <chrono>
#include <iostream>

namespace ScreenMotionTestDetail {
inline cv::Mat Warp(const cv::Mat& source, double scale, double x, double y) {
    cv::Mat result;
    const cv::Mat transform = (cv::Mat_<double>(2, 3) << scale, 0, x, 0, scale, y);
    cv::warpAffine(source, result, transform, source.size(), cv::INTER_LINEAR, cv::BORDER_REFLECT101);
    return result;
}

inline void Heading(cv::Mat& image, int degrees) {
    const cv::Point center(image.cols / 2, image.rows / 2);
    const double angle = degrees * CV_PI / 180.0;
    const double radius = std::min(image.rows, image.cols) * 0.46;
    std::vector<cv::Point> cone{center};
    for (double delta : {-0.22, 0.22}) cone.emplace_back(cvRound(center.x + radius * std::cos(angle + delta)),
        cvRound(center.y + radius * std::sin(angle + delta)));
    cv::Mat veil = image.clone();
    cv::fillConvexPoly(veil, cone, cv::Scalar(105, 115, 120));
    cv::addWeighted(image, 0.66, veil, 0.34, 0, image);
    cv::circle(image, center, std::min(image.rows, image.cols) / 12, cv::Scalar(0, 230, 255), cv::FILLED);
}
}

template<class Check>
void TestRapidTerrainMotion(const cv::Mat& gameCapture, Check check) {
    if (gameCapture.empty()) return;
    cv::Mat panorama;
    cv::resize(gameCapture, panorama, {1600, 900}, 0, 0, cv::INTER_AREA);
    if (panorama.channels() > 1) cv::cvtColor(panorama, panorama, cv::COLOR_BGR2GRAY);
    struct Pose { double x, y, scale; };
    const std::vector<Pose> path{
        {0, 0, 1}, {40, -10, 1}, {100, -20, 1}, {160, 10, 1}, {210, 35, 1.08},
        {210, 35, 1.08}, {130, 20, 1.08}, {40, -10, 0.96}, {-60, -25, 0.96},
        {-140, 10, 1.04}, {-140, 10, 1.04}, {-50, 20, 1.04}, {60, 0, 1.10},
        {150, -20, 1.10}, {60, 10, 1}, {0, 0, 1}
    };
    const auto sample = [](const cv::Mat& terrain, const Pose& pose) {
        // Every pixel comes from the larger real screenshot, including newly
        // revealed terrain. Reflected/padded copies cannot fake drag overlap.
        const cv::Mat transform = (cv::Mat_<double>(2, 3) << pose.scale, 0, 175 + pose.x - 800 * pose.scale,
            0, pose.scale, 175 + pose.y - 450 * pose.scale);
        cv::Mat view;
        cv::warpAffine(terrain, view, transform, {350, 350}, cv::INTER_LINEAR, cv::BORDER_CONSTANT);
        return view;
    };
    for (double contrast : {1.0, 0.15}) {
        cv::Mat terrain;
        panorama.convertTo(terrain, CV_8UC1, contrast, contrast == 1.0 ? 0 : 60);
        ScreenMotionTracker tracker;
        check(tracker.Anchor(sample(terrain, path.front()), false), "rapid terrain replay has a valid initial anchor");
        int rejected = 0, wrongAccepted = 0;
        double maximumError = 0, maximumScaleError = 0;
        const auto started = std::chrono::steady_clock::now();
        for (size_t frame = 1; frame < path.size(); ++frame) {
            OverlayScreenTransform measured;
            if (!tracker.Track(sample(terrain, path[frame]), measured)) { ++rejected; continue; }
            const auto center = measured.Apply({175, 175});
            const double error = std::hypot(center.x - 175 - path[frame].x, center.y - 175 - path[frame].y);
            const double scaleError = std::abs(measured.scale - path[frame].scale) * 350;
            maximumError = std::max(maximumError, error);
            maximumScaleError = std::max(maximumScaleError, scaleError);
            if (error > 1.5 || scaleError > 1.5) ++wrongAccepted;
        }
        std::cout << "Rapid real terrain contrast=" << contrast << " rejected=" << rejected
            << " wrongAccepted=" << wrongAccepted << " maxCenterError=" << maximumError
            << " maxScalePixelError=" << maximumScaleError << " averageFrameMs="
            << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() / (path.size() - 1) << '\n';
        check(wrongAccepted == 0, "large drag and sudden reversal never accept an obsolete low-motion pose on low-contrast terrain");
        if (contrast == 1.0) check(rejected == 0,
            "overlapping real terrain remains attached through 40-110px frame steps, stops and simultaneous zoom");
        // Low contrast may not carry enough texture for reacquisition; rejection
        // is safe, but accepting a near-zero transform for a100px drag is not.
    }
}

template<class Check>
void TestScreenMotionTracker(Check check) {
    using namespace ScreenMotionTestDetail;
    cv::Mat synthetic(320, 320, CV_8UC1);
    cv::RNG random(912);
    random.fill(synthetic, cv::RNG::UNIFORM, 30, 150);
    cv::GaussianBlur(synthetic, synthetic, {5, 5}, 0.8);
    for (int i = 0; i < 110; ++i) cv::circle(synthetic,
        {random.uniform(15, 305), random.uniform(15, 305)}, random.uniform(2, 8), cv::Scalar(random.uniform(40, 190)), 2);
    std::vector<std::pair<std::string, cv::Mat>> fixtures{{"synthetic", synthetic}};
#ifdef IMAO_SOURCE_DIR
    const auto bigmap = cv::imread(std::string(IMAO_SOURCE_DIR) + "/Tests/VisualLocalization/20260906-regression/bigmap.png");
    check(!bigmap.empty(), "screen motion real terrain fixture is available");
    if (!bigmap.empty()) {
        const cv::Rect region(cvRound(bigmap.cols * 625.0 / 1600), cvRound(bigmap.rows * 275.0 / 900),
            cvRound(bigmap.cols * 350.0 / 1600), cvRound(bigmap.rows * 350.0 / 900));
        fixtures.emplace_back("real-terrain", bigmap(region).clone());
    }
    TestRapidTerrainMotion(bigmap, check);
#endif
    for (const auto& fixture : fixtures) {
        for (bool zoom : {false, true}) {
            ScreenMotionTracker tracker;
            check(tracker.Anchor(fixture.second, false), "terrain tracking accepts a textured image anchor");
            OverlayScreenTransform measured;
            double worstError = 0, worstScaleError = 0;
            int failures = 0;
            const auto started = std::chrono::steady_clock::now();
            for (int frame = 0; frame < 80; ++frame) {
                // Includes instantaneous stop, restart, reverse and more than
                // one reference rebase. No intermediate animation is allowed.
                const double distance = frame < 28 ? frame * 0.85 : frame < 38 ? 27 * 0.85 : (65 - frame) * 0.85;
                const double scale = zoom ? 1.0 + std::max(0.0, distance) * 0.004 : 1.0;
                const double x = distance + fixture.second.cols * (1 - scale) / 2;
                const double y = -distance * 0.35 + fixture.second.rows * (1 - scale) / 2;
                if (!tracker.Track(Warp(fixture.second, scale, x, y), measured)) {
                    ++failures;
                    std::cout << "Screen motion rejected fixture=" << fixture.first << " zoom=" << zoom << " frame=" << frame << '\n';
                    continue;
                }
                const auto center = measured.Apply({fixture.second.cols / 2.0, fixture.second.rows / 2.0});
                worstError = std::max(worstError, std::hypot(center.x - (fixture.second.cols / 2.0 + distance),
                    center.y - (fixture.second.rows / 2.0 - distance * 0.35)));
                worstScaleError = std::max(worstScaleError, std::abs(scale - measured.scale) * fixture.second.cols);
            }
            const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() / 80;
            std::cout << "Screen motion fixture=" << fixture.first << " zoom=" << zoom << " failed=" << failures
                << " maxCenterError=" << worstError << " maxScalePixelError=" << worstScaleError << " averageFrameMs=" << milliseconds << '\n';
            check(failures == 0 && worstError < 0.85 && worstScaleError < 1.0,
                "pan zoom stop and reversal follow observed terrain within one pixel without easing");
            ScreenMotionTracker abrupt;
            abrupt.Anchor(fixture.second, false);
            const double abruptScale = 1.08;
            const double abruptX = 25 + fixture.second.cols * (1 - abruptScale) / 2;
            const double abruptY = -11 + fixture.second.rows * (1 - abruptScale) / 2;
            const bool abruptTracked = abrupt.Track(Warp(fixture.second, abruptScale, abruptX, abruptY), measured);
            const auto abruptCenter = measured.Apply({fixture.second.cols / 2.0, fixture.second.rows / 2.0});
            const double abruptError = std::hypot(abruptCenter.x - fixture.second.cols / 2.0 - 25,
                abruptCenter.y - fixture.second.rows / 2.0 + 11);
            check(abruptTracked && abruptError < 1.0 && std::abs(measured.scale - abruptScale) * fixture.second.cols < 1.0,
                "abrupt large pan and zoom use measured image fallback without a transition animation");
            std::cout << "Screen abrupt fixture=" << fixture.first << " tracked=" << abruptTracked
                << " centerError=" << abruptError << " scalePixelError=" << std::abs(measured.scale - abruptScale) * fixture.second.cols << '\n';
            check(!tracker.Track(cv::Mat::zeros(fixture.second.size(), fixture.second.type()), measured),
                "textureless capture cannot produce a motion transform");
            check(!tracker.Track(cv::Mat::zeros(80, 120, CV_8UC3), measured),
                "changed capture geometry invalidates screen tracking");
            tracker.Reset();
            check(!tracker.Track(fixture.second, measured), "a reset never reuses previous interface motion");
        }
    }

    std::vector<cv::Mat> minimaps;
    cv::Mat colored;
    cv::cvtColor(synthetic, colored, cv::COLOR_GRAY2BGR);
    minimaps.push_back(colored);
#ifdef IMAO_SOURCE_DIR
    for (const auto* filename : {"1_minimap-feature-source.png", "19_minimap-feature-source.png"}) {
        const auto image = cv::imread(std::string(IMAO_SOURCE_DIR) + "/Tests/VisualLocalization/20260907-cross-area/" + filename);
        check(!image.empty(), "real minimap fixture is available for animation regression");
        if (!image.empty()) minimaps.push_back(image);
    }
#endif
    for (size_t fixture = 0; fixture < minimaps.size(); ++fixture) {
        const auto& original = minimaps[fixture];
        auto first = original.clone(); Heading(first, 0);
        ScreenMotionTracker tracker;
        check(tracker.Anchor(first, true), "minimap anchor uses terrain outside player and heading graphics");
        OverlayScreenTransform measured;
        double worstError = 0;
        int failures = 0;
        const auto started = std::chrono::steady_clock::now();
        for (int frame = 0; frame < 60; ++frame) {
            const double x = frame < 20 ? 0 : frame < 40 ? (frame - 20) * 0.45 : (59 - frame) * 0.45;
            const double y = -x * 0.4;
            auto current = Warp(original, 1.0, x, y);
            Heading(current, frame * 19);
            if (!tracker.Track(current, measured)) {
                ++failures;
                std::cout << "Screen minimap rejected fixture=" << fixture << " frame=" << frame << '\n';
                continue;
            }
            worstError = std::max(worstError, std::hypot(measured.offset.x - x, measured.offset.y - y));
            check(measured.scale == 1.0, "camera heading never changes minimap marker scale");
        }
        const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() / 60;
        std::cout << "Screen minimap fixture=" << fixture << " failed=" << failures << " maxError=" << worstError
            << " averageFrameMs=" << milliseconds << '\n';
        check(failures == 0 && worstError < 0.85,
            "moving player and rotating translucent heading cone do not displace stationary minimap terrain");
        const auto stationary = first.clone();
        if (tracker.Track(stationary, measured)) {
            const auto stable = measured;
            for (int frame = 0; frame < 12; ++frame) tracker.Track(stationary, measured);
            check(std::hypot(measured.offset.x - stable.offset.x, measured.offset.y - stable.offset.y) < 0.03,
                "unchanged pixels do not continue moving or accumulate a tracking tail");
        } else check(false, "minimap can immediately return to its stationary anchor");
    }
}
