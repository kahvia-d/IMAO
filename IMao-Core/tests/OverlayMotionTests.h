#pragma once
#include "Runtime/OverlayMotion.h"
#include "App/MapViewportPredictor.h"
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <iostream>
#include <limits>


template<class Check>
void TestFineViewportMotion(Check check) {
    cv::Mat source = cv::Mat::zeros(256, 256, CV_8UC1);
    cv::RNG random(907);
    for (int i = 0; i < 180; ++i) {
        cv::circle(source, {random.uniform(12, 244), random.uniform(12, 244)},
            random.uniform(2, 6), cv::Scalar(random.uniform(80, 255)), cv::FILLED);
    }
    const auto begin = std::chrono::steady_clock::now();
    std::vector<std::pair<std::string, cv::Mat>> fixtures{{"synthetic", source}};
#ifdef IMAO_SOURCE_DIR
    const cv::Mat capture = cv::imread(std::string(IMAO_SOURCE_DIR) + "/Tests/VisualLocalization/20260906-regression/bigmap.png", cv::IMREAD_GRAYSCALE);
    check(!capture.empty(), "real game map fixture is available for motion regression");
    if (!capture.empty()) {
        const cv::Rect region(cvRound(capture.cols * 625.0 / 1600), cvRound(capture.rows * 275.0 / 900),
            cvRound(capture.cols * 350.0 / 1600), cvRound(capture.rows * 350.0 / 900));
        cv::Mat terrain;
        cv::resize(capture(region), terrain, cv::Size(256, 256), 0, 0, cv::INTER_AREA);
        fixtures.emplace_back("game-terrain", terrain);
    }
#endif
    for (const auto& fixture : fixtures) {
    source = fixture.second;
    {
        MapViewportPredictor validity;
        MapViewportPrediction pose;
        validity.ObserveFrame(source, pose);
        validity.Confirm(1, {1000, 2000}, {{872, 1872}, {1128, 1872}, {1128, 2128}, {872, 2128}});
        check(validity.IsCurrentFrameAnchored(), "absolute confirmation anchors its observed image");
        const bool stationaryChanged = validity.ObserveFrame(source, pose);
        check(!stationaryChanged && validity.IsCurrentFrameAnchored(),
            "verified stationary viewport is anchored although its position did not change");
        validity.ObserveFrame(cv::Mat::zeros(source.size(), source.type()), pose);
        validity.GetPrediction(pose);
        check(!validity.IsCurrentFrameAnchored() && pose.confidence >= 2,
            "first tracking failure revokes current image binding before confidence grace expires");
        validity.ObserveFrame(source, pose);
        check(validity.IsCurrentFrameAnchored(), "valid terrain recovers the original anchor after a failed frame");
        validity.ObserveFrame(cv::Mat(), pose);
        check(!validity.IsCurrentFrameAnchored(), "empty capture cannot inherit prior viewport binding");
    }
    for (bool withZoom : {false, true}) {
        MapViewportPredictor predictor;
        predictor.Confirm(1, {1000, 2000}, {{872, 1872}, {1128, 1872}, {1128, 2128}, {872, 2128}});
        MapViewportPrediction result;
        predictor.ObserveFrame(source, result);
        for (int i = 1; i <= 40; ++i) {
            const double scale = withZoom ? std::pow(1.001, i) : 1.0;
            // Every individual translation and zoom is below the old cutoff.
            const cv::Mat transform = (cv::Mat_<double>(2, 3) << scale, 0, 128 * (1 - scale) - i * 0.2,
                0, scale, 128 * (1 - scale) + i * 0.1);
            cv::Mat current;
            cv::warpAffine(source, current, transform, source.size(), cv::INTER_LINEAR, cv::BORDER_REFLECT101);
            predictor.ObserveFrame(current, result);
        }
        predictor.GetPrediction(result);
        const double scale = withZoom ? std::pow(1.001, 40) : 1.0;
        const double error = std::hypot(result.centerMapCoordinate.x - (1000 + 8 / scale),
            result.centerMapCoordinate.y - (2000 - 4 / scale));
        const double width = cv::norm(result.captureCorners[1] - result.captureCorners[0]);
        check(error < 0.8 && std::abs(width - 256 / scale) < 0.8,
            "subpixel pan and small zoom accumulate with less than one tracking-pixel error");
        const auto stable = result.centerMapCoordinate;
        cv::Mat stationary;
        const cv::Mat finalTransform = (cv::Mat_<double>(2, 3) << scale, 0, 128 * (1 - scale) - 8,
            0, scale, 128 * (1 - scale) + 4);
        cv::warpAffine(source, stationary, finalTransform, source.size(), cv::INTER_LINEAR, cv::BORDER_REFLECT101);
        for (int i = 0; i < 20; ++i) predictor.ObserveFrame(stationary, result);
        predictor.GetPrediction(result);
        check(std::hypot(result.centerMapCoordinate.x - stable.x, result.centerMapCoordinate.y - stable.y) < 0.15,
            "stationary terrain does not accumulate subpixel tracking drift");
        std::cout << "Viewport fine motion fixture=" << fixture.first << " zoom=" << withZoom << " centerError=" << error
            << " widthError=" << std::abs(width - 256 / scale) << '\n';
    }
    MapViewportPredictor fast;
    fast.Confirm(1, {1000, 2000}, {{872, 1872}, {1128, 1872}, {1128, 2128}, {872, 2128}});
    MapViewportPrediction result;
    fast.ObserveFrame(source, result);
    cv::Mat jumped;
    const cv::Mat jump = (cv::Mat_<double>(2, 3) << 1.10, 0, -12.8 - 20, 0, 1.10, -12.8 + 8);
    cv::warpAffine(source, jumped, jump, source.size(), cv::INTER_LINEAR, cv::BORDER_REFLECT101);
    const bool tracked = fast.ObserveFrame(jumped, result);
    check(tracked && std::hypot(result.centerMapCoordinate.x - (1000 + 20 / 1.1),
        result.centerMapCoordinate.y - (2000 - 8 / 1.1)) < 2.0,
        "descriptor fallback preserves larger simultaneous drag and zoom steps");
    MapViewportPrediction requestPose;
    requestPose.sceneId = 1;
    requestPose.centerMapCoordinate = {1000, 2000};
    requestPose.captureCorners = {{872, 1872}, {1128, 1872}, {1128, 2128}, {872, 2128}};
    MapViewportPrediction bridged;
    check(MapViewportPredictor::TryBridgePrediction(source, source, requestPose, bridged) &&
        std::hypot(bridged.centerMapCoordinate.x - 1000, bridged.centerMapCoordinate.y - 2000) < 0.1,
        "late stationary absolute result can be verified against the current image");
    check(MapViewportPredictor::TryBridgePrediction(source, jumped, requestPose, bridged) &&
        std::hypot(bridged.centerMapCoordinate.x - (1000 + 20 / 1.1),
            bridged.centerMapCoordinate.y - (2000 - 8 / 1.1)) < 2.0 &&
        std::abs(cv::norm(bridged.captureCorners[1] - bridged.captureCorners[0]) - 256 / 1.1) < 2.0,
        "late absolute result is translated and scaled to its target frame before confirmation");
    requestPose.revision = 4;
    bridged.revision = 19;
    check(MapViewportPredictor::CanConfirmAfterBridge(source, jumped, requestPose, bridged, 2.0, 0.15),
        "independent viewport candidates agree after motion despite different prediction revisions");
    auto wrongPending = requestPose;
    wrongPending.centerMapCoordinate.x += 80;
    for (auto& corner : wrongPending.captureCorners) corner.x += 80;
    check(!MapViewportPredictor::CanConfirmAfterBridge(source, jumped, wrongPending, bridged, 36.0, 0.15),
        "motion bridging cannot make an incorrect absolute candidate pass center confirmation");
    auto wrongScale = bridged;
    for (auto& corner : wrongScale.captureCorners) {
        corner.x = static_cast<float>(wrongScale.centerMapCoordinate.x +
            (corner.x - wrongScale.centerMapCoordinate.x) * 1.25);
        corner.y = static_cast<float>(wrongScale.centerMapCoordinate.y +
            (corner.y - wrongScale.centerMapCoordinate.y) * 1.25);
    }
    check(!MapViewportPredictor::CanConfirmAfterBridge(source, jumped, requestPose, wrongScale, 36.0, 0.15),
        "motion compensated confirmation rejects an independently incorrect viewport scale");
    check(!MapViewportPredictor::TryBridgePrediction(source, cv::Mat::zeros(source.size(), source.type()),
        requestPose, bridged) && bridged.sceneId == 0 && bridged.captureCorners.empty(),
        "unbridgeable absolute result cannot leave a stale pose labeled as current");
    check(!MapViewportPredictor::TryBridgePrediction(source, cv::Mat::zeros(128, 128, source.type()),
        requestPose, bridged), "viewport bridge rejects capture size changes");
    for (int i = 0; i < 6; ++i) fast.ObserveFrame(cv::Mat::zeros(256, 256, CV_8UC1), result);
    fast.GetPrediction(result);
    check(result.confidence < 2, "texture loss revokes predicted viewport confidence");
    }
    std::cout << "Viewport motion replay durationMs=" <<
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count() << '\n';
}
