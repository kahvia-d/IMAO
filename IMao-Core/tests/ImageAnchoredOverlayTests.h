#pragma once
#include "Runtime/ImageAnchoredOverlay.h"
#include "Coordinate/locationCalculator/MinimapProjectionGeometry.h"
#include <opencv2/imgproc.hpp>
#include <deque>

template<class Check>
void TestLongImageAnchoredOverlay(Check check) {
    using namespace std::chrono;
    constexpr int period = 600;
    constexpr int totalFrames = 12000; // 192 seconds of 60 Hz captures per mode.
    const auto started = steady_clock::now();
    const RECT viewport{0, 0, 160, 160};
    const cv::Rect region(0, 0, 160, 160);
    const Coordinate screenCenter(80, 80), worldPoint(1021, 986);
    cv::Mat terrain(160, 160, CV_8UC1);
    cv::RNG random(9281);
    random.fill(terrain, cv::RNG::UNIFORM, 30, 170);
    cv::GaussianBlur(terrain, terrain, {5, 5}, 0.8);

    for (bool minimap : {false, true}) {
        ImageAnchoredOverlay overlay(minimap);
        std::deque<cv::Mat> recentImages;
        std::deque<OverlayMotionSample> recentPoses;
        std::shared_ptr<OverlayFrame> source;
        OverlayScreenTransform motion;
        double maximumError = 0, maximumReturnError = 0;
        int failures = 0, returns = 0;
        const auto clockStart = steady_clock::now();
        for (int frame = 0; frame <= totalFrames; ++frame) {
            const int phase = frame % period;
            const double x = (phase <= period / 2 ? phase : period - phase) * 0.04;
            const double y = 2.5 * std::sin(phase * 2.0 * CV_PI / period);
            const double scale = minimap ? 1.0 : 1.0 + 0.006 * std::sin(phase * 2.0 * CV_PI / period);
            cv::Mat image;
            const cv::Mat affine = (cv::Mat_<double>(2, 3) << scale, 0, 80 * (1 - scale) + x,
                0, scale, 80 * (1 - scale) + y);
            cv::warpAffine(terrain, image, affine, terrain.size(), cv::INTER_LINEAR, cv::BORDER_REFLECT101);
            const OverlayMotionSample pose{{1000 - x / scale, 1000 - y / scale}, screenCenter, scale,
                1, 1, static_cast<uint64_t>(frame + 1), started + milliseconds(frame * 16), true};
            recentImages.push_back(image);
            recentPoses.push_back(pose);
            if (recentImages.size() > 3) { recentImages.pop_front(); recentPoses.pop_front(); }
            if (frame % 5 == 0) {
                source = std::make_shared<OverlayFrame>();
                auto noisy = recentPoses.front();
                if (frame > 0) {
                    noisy.center.x += 1.3 * std::sin(frame * 0.071);
                    noisy.center.y += 0.8 * std::cos(frame * 0.113);
                }
                source->frameId = noisy.frameId;
                source->capturedAt = noisy.capturedAt;
                source->clientRect = viewport;
                source->motionRegion = region;
                source->motionImage = recentImages.front();
                source->mapMotion = source->minimapMotion = noisy;
            }
            const CapturedFrame capture{static_cast<uint64_t>(frame + 1), image, viewport,
                pose.capturedAt, milliseconds(250)};
            if (!overlay.Update(source, capture, motion, pose.capturedAt)) { ++failures; continue; }
            const auto& anchor = source->mapMotion;
            const Coordinate raw(screenCenter.x + (worldPoint.x - anchor.center.x) * anchor.pixelsPerUnit,
                screenCenter.y + (worldPoint.y - anchor.center.y) * anchor.pixelsPerUnit);
            const auto drawn = motion.Apply(raw);
            const Coordinate expected(screenCenter.x + (worldPoint.x - 1000) * scale + x,
                screenCenter.y + (worldPoint.y - 1000) * scale + y);
            const double error = std::hypot(drawn.x - expected.x, drawn.y - expected.y);
            maximumError = std::max(maximumError, error);
            if (frame > 0 && phase == 0) {
                maximumReturnError = std::max(maximumReturnError, error);
                ++returns;
            }
        }
        check(failures == 0 && maximumError < 0.6,
            "three-minute slow motion does not integrate bias across thousands of noisy localization anchors");
        check(returns == totalFrames / period && maximumReturnError < 0.2,
            "repeated closed image-motion loops return markers to their original positions without accumulated drift");
        std::cout << "Long image anchor minimap=" << minimap << " simulatedSeconds=" << totalFrames * 0.016
            << " failures=" << failures << " maxError=" << maximumError << " maxReturnError=" << maximumReturnError
            << " elapsedMs=" << duration<double, std::milli>(steady_clock::now() - clockStart).count() << '\n';
    }
}

template<class Check>
void TestOverlayFailureContinuity(Check check) {
    using namespace std::chrono;
    const auto start = steady_clock::now();
    const RECT viewport{0, 0, 320, 320};
    cv::Mat terrain(320, 320, CV_8UC1);
    cv::RNG random(9208);
    random.fill(terrain, cv::RNG::UNIFORM, 35, 155);
    cv::GaussianBlur(terrain, terrain, {5, 5}, 0.8);
    auto source = std::make_shared<OverlayFrame>();
    source->frameId = 1; source->capturedAt = start; source->clientRect = viewport;
    source->focused = true; source->playerScene = 1; source->minimapVisible = true;
    source->motionImage = terrain; source->motionRegion = {0, 0, 320, 320};
    source->mapMotion = source->minimapMotion = {{0, 0}, {160, 160}, 1, 1, 1, 1, start, true};
    OverlayFrame pending = *source;
    pending.frameId = 3; pending.capturedAt = start + milliseconds(100); pending.minimapMotion.reliable = false;
    check(CanRetainOverlayAnchor(*source, pending, false, true, pending.capturedAt),
        "temporary localization loss retains the original image/pose without a new timestamp");
    check(!CanRetainOverlayAnchor(*source, pending, true, false, pending.capturedAt) &&
        !CanRetainOverlayAnchor(*source, pending, false, true, start + milliseconds(501)),
        "holding an anchor cannot cross a map transition or refresh its expiry");
    pending.clientRect.right = 640;
    check(!CanRetainOverlayAnchor(*source, pending, false, true, pending.capturedAt),
        "a resized capture cannot reuse the old HUD geometry");

    ImageAnchoredOverlay overlay(false);
    source->mapVisible = true; source->minimapVisible = false; source->mapMotion.absoluteRevision = 1;
    std::deque<cv::Mat> images;
    OverlayScreenTransform motion;
    CapturedFrame capture;
    double maximumError = 0;
    int failures = 0;
    for (int i = 0; i <= 20; ++i) {
        const double shift = i * 2.5;
        cv::Mat current;
        const cv::Mat affine = (cv::Mat_<double>(2, 3) << 1, 0, shift, 0, 1, 0);
        cv::warpAffine(terrain, current, affine, terrain.size(), cv::INTER_LINEAR, cv::BORDER_REFLECT101);
        images.push_back(current);
        if (images.size() > 3) images.pop_front();
        if (i > 0 && i % 5 == 0) {
            source = std::make_shared<OverlayFrame>(*source);
            source->frameId = i - 1; source->capturedAt = start + milliseconds((i - 2) * 16);
            source->motionImage = images.front();
            source->mapMotion.capturedAt = source->capturedAt;
            source->mapMotion.frameId = source->frameId;
            // Deliberately stale low-rate prediction attached to a newer image.
            // Strong current terrain evidence must win, even beyond12px.
        }
        capture = {static_cast<uint64_t>(i + 1), current, viewport, start + milliseconds(i * 16), milliseconds(250)};
        if (!overlay.Update(source, capture, motion, capture.capturedAt)) { ++failures; continue; }
        maximumError = std::max(maximumError, std::hypot(motion.Apply({160, 160}).x - (160 + shift),
            motion.Apply({160, 160}).y - 160));
    }
    check(failures == 0 && maximumError < 0.4,
        "fast terrain motion cannot snap to a stale prediction when disagreement exceeds12px");
    std::cout << "Stale viewport prediction failures=" << failures << " maxError=" << maximumError << '\n';
    OverlayScreenTransform repeated;
    check(overlay.Update(source, capture, repeated, capture.capturedAt + milliseconds(140)) &&
        std::hypot(repeated.offset.x - motion.offset.x, repeated.offset.y - motion.offset.y) < 1e-9,
        "valid slow capture does not blink at an unrelated100ms deadline or animate while waiting");
    check(!overlay.Update(source, capture, repeated, capture.capturedAt + milliseconds(250)),
        "a genuinely stalled capture expires at its shared freshness deadline");
    auto newAbsolute = source->mapMotion;
    newAbsolute.absoluteRevision = 2;
    const auto independent = ReconcileTerrainAnchor(source->mapMotion, newAbsolute, {1, {40, 0}}, {}, false);
    check(independent.offset.x == 0,
        "a new independently confirmed absolute correction can replace a drifting terrain anchor");
}

template<class Check>
void TestImageAnchoredOverlay(Check check) {
    using namespace std::chrono;
    const auto start = steady_clock::now();
    const RECT rect{0, 0, 640, 480};
    const cv::Rect region(100, 50, 320, 320);
    const Coordinate screenCenter(260, 210), worldPoint(1020, 1010);
    cv::Mat terrain(320, 320, CV_8UC1);
    cv::RNG random(9078);
    random.fill(terrain, cv::RNG::UNIFORM, 35, 155);
    cv::GaussianBlur(terrain, terrain, {5, 5}, 0.8);
    for (bool mini : {false, true}) {
        ImageAnchoredOverlay overlay(mini);
        std::vector<cv::Mat> images;
        std::vector<OverlayMotionSample> poses;
        std::shared_ptr<OverlayFrame> source;
        OverlayScreenTransform motion;
        double maximumError = 0;
        int failures = 0;
        for (int i = 0; i < 50; ++i) {
            // The 24.7px excursion crosses the tracker's 18px internal rebase
            // threshold in both directions while source updates arrive late.
            const double shift = i < 20 ? i * 1.3 : i < 28 ? 19 * 1.3 : (47 - i) * 1.3;
            const double scale = mini ? 1.0 : 1.0 + shift * 0.003;
            const cv::Mat affine = (cv::Mat_<double>(2, 3) << scale, 0, 160 * (1 - scale) + shift,
                0, scale, 160 * (1 - scale) - shift * 0.3);
            cv::Mat image;
            cv::warpAffine(terrain, image, affine, terrain.size(), cv::INTER_LINEAR, cv::BORDER_REFLECT101);
            images.push_back(image);
            OverlayMotionSample pose{{1000 - shift / scale, 1000 + shift * 0.3 / scale}, screenCenter,
                scale, 1, 1, static_cast<uint64_t>(i + 1), start + milliseconds(i * 16), true};
            poses.push_back(pose);
            // Absolute positions arrive every80ms, already32ms behind capture,
            // with changing +/-1.3px feature-fit error. Pixel motion is current.
            if (i == 0 || i % 5 == 0) {
                const int index = i == 0 ? 0 : i - 2;
                source = std::make_shared<OverlayFrame>();
                source->frameId = index + 1; source->clientRect = rect;
                source->capturedAt = poses[index].capturedAt;
                source->motionRegion = region; source->motionImage = images[index];
                auto noisy = poses[index];
                if (i > 0) { noisy.center.x += 1.3 * std::sin(i); noisy.center.y += 0.8 * std::cos(i); }
                source->mapMotion = source->minimapMotion = noisy;
            }
            cv::Mat full = cv::Mat::zeros(rect.bottom, rect.right, CV_8UC1);
            image.copyTo(full(region));
            const CapturedFrame capture{static_cast<uint64_t>(i + 1), full, rect, pose.capturedAt, milliseconds(250)};
            if (!overlay.Update(source, capture, motion, pose.capturedAt)) { ++failures; continue; }
            const auto& anchor = source->mapMotion;
            const Coordinate raw(screenCenter.x + (worldPoint.x - anchor.center.x) * anchor.pixelsPerUnit,
                screenCenter.y + (worldPoint.y - anchor.center.y) * anchor.pixelsPerUnit);
            const auto shown = motion.Apply(raw);
            const Coordinate expected(screenCenter.x + 20 * scale + shift, screenCenter.y + 10 * scale - shift * 0.3);
            maximumError = std::max(maximumError, std::hypot(shown.x - expected.x, shown.y - expected.y));
            const auto clicked = motion.Inverse(shown);
            check(std::hypot(clicked.x - raw.x, clicked.y - raw.y) < 1e-6, "image-attached input maps back to the same marker");
            OverlayScreenTransform repeated;
            check(overlay.Update(source, capture, repeated, pose.capturedAt + milliseconds(60)) &&
                std::hypot(repeated.Apply(raw).x - shown.x, repeated.Apply(raw).y - shown.y) < 1e-6,
                "waiting without new pixels adds no animation or drift");
        }
        check(failures == 0 && maximumError < 0.6,
            "current terrain stays attached despite delayed noisy absolute positions, stops and reversals");
        std::cout << "Image anchor integration minimap=" << mini << " failures=" << failures << " maxError=" << maximumError << '\n';
        source->mapMotion.reliable = source->minimapMotion.reliable = false;
        const CapturedFrame current{50, cv::Mat::zeros(480, 640, CV_8UC1), rect, start + milliseconds(800), milliseconds(250)};
        check(!overlay.Update(source, current, motion, start + milliseconds(800)), "untrusted localization cannot animate old markers");
        source->mapMotion.reliable = source->minimapMotion.reliable = true;
        check(!overlay.Update(source, current, motion, start + milliseconds(1051)), "stalled capture cannot keep image-attached markers alive");
    }

    for (int width : {1280, 1600, 1920, 2560, 3840}) {
        const RECT viewport{0, 0, width, width * 9 / 16};
        for (double calibration : {kNominalMinimapTerrainScale, 1.075}) {
            const auto geometry = GetMinimapProjectionGeometry(viewport, calibration);
            check(std::abs(geometry.pixelsPerMapUnit * (184 * calibration) - geometry.width) < 1e-9 &&
                geometry.center.x == geometry.left + geometry.width / 2.0 && geometry.center.y == geometry.top + geometry.height / 2.0,
                "minimap projection exactly matches the integer localization crop at every resolution");
        }
    }
    TestLongImageAnchoredOverlay(check);
    TestOverlayFailureContinuity(check);
}
