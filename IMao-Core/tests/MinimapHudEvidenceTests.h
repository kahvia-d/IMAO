#pragma once

#include "App/MinimapHudEvidence.h"
#include <opencv2/imgcodecs.hpp>
#include <filesystem>
#include <iostream>

template<class Check>
void TestMinimapHudEvidence(Check check) {
    const auto taskRoi = [](cv::Size size) {
        return cv::Rect(static_cast<int>(12.0 * size.width / 1600.0),
            static_cast<int>(183.0 * size.height / 900.0),
            static_cast<int>(27.0 * size.width / 1600.0),
            static_cast<int>(24.0 * size.height / 900.0));
    };
#ifdef IMAO_SOURCE_DIR
    const auto root = std::filesystem::path(IMAO_SOURCE_DIR);
    const auto gameplay = cv::imread((root / "Tests/MapUi/black-shores-gameplay.png").string());
    const auto map = cv::imread((root / "Tests/MapUi/black-shores-map.png").string());
    check(!gameplay.empty() && !map.empty(), "HUD evidence real gameplay and map captures load");
    if (!gameplay.empty() && !map.empty()) {
        for (const auto size : {cv::Size(1280, 720), cv::Size(1600, 900),
                cv::Size(1920, 1080), cv::Size(2560, 1440)}) {
            cv::Mat positive, negative;
            cv::resize(gameplay, positive, size);
            cv::resize(map, negative, size);
            MinimapHudEvidence evidence;
            const auto roi = taskRoi(size);
            check(!evidence.Observe(positive, roi, false, true, false).visible,
                "weak task icon evidence cannot initialize its own template");
            const auto learned = evidence.Observe(positive, roi, true, true, false);
            const auto fallback = evidence.Observe(positive, roi, false, true, false);
            check(learned.templateReady && fallback.visible && fallback.usedTemplate,
                "real task glyph survives a missing SURF match at width " + std::to_string(size.width));
            check(!evidence.Observe(negative, roi, false, true, false).visible,
                "real big-map pixels reject the cached gameplay glyph immediately");
            check(evidence.Observe(positive, roi, false, true, false).visible,
                "rejected fallback pixels do not replace the independently learned glyph");
            cv::Mat brightBackground = positive.clone();
            brightBackground(roi).setTo(cv::Scalar(240, 240, 240));
            check(!evidence.Observe(brightBackground, roi, false, true, false).visible,
                "bright background without the glyph's dark details is rejected immediately");
            cv::Mat bgra;
            cv::cvtColor(positive, bgra, cv::COLOR_BGR2BGRA);
            check(evidence.Observe(bgra, roi, false, true, false).visible,
                "BGRA capture preserves task glyph evidence");
            std::cout << "HUD glyph width=" << size.width << " foreground="
                << fallback.foregroundRecall << " shape=" << fallback.shapeAgreement << '\n';
        }
    }
#endif

    const cv::Size canvas(320, 180);
    const cv::Rect roi(12, 24, 54, 48);
    const auto synthetic = [&](int backgroundPhase, bool icon, int glyphOffset = 0) {
        cv::Mat result(canvas, CV_8UC3);
        for (int y = 0; y < result.rows; ++y) {
            for (int x = 0; x < result.cols; ++x) {
                result.at<cv::Vec3b>(y, x) = cv::Vec3b(
                    static_cast<unsigned char>(30 + (x * 3 + backgroundPhase * 7) % 170),
                    static_cast<unsigned char>(15 + (y * 5 + backgroundPhase * 11) % 110),
                    static_cast<unsigned char>(20 + (x + y + backgroundPhase * 13) % 90));
            }
        }
        if (icon) {
            const auto origin = roi.tl() + cv::Point(glyphOffset, 0);
            cv::rectangle(result, cv::Rect(origin + cv::Point(9, 5), cv::Size(31, 39)), cv::Scalar(245, 245, 245), cv::FILLED);
            cv::rectangle(result, cv::Rect(origin + cv::Point(15, 15), cv::Size(17, 5)), cv::Scalar(25, 25, 25), cv::FILLED);
            cv::rectangle(result, cv::Rect(origin + cv::Point(15, 27), cv::Size(17, 4)), cv::Scalar(25, 25, 25), cv::FILLED);
        }
        return result;
    };
    MinimapHudEvidence evidence;
    const auto first = synthetic(0, true);
    check(evidence.Observe(first, roi, true, true, false).templateReady,
        "HUD glyph template learns only from independent positive evidence");
    bool movingBackgroundAccepted = true;
    for (int frame = 1; frame <= 60; ++frame) {
        movingBackgroundAccepted = movingBackgroundAccepted &&
            evidence.Observe(synthetic(frame, true), roi, false, true, false).visible;
    }
    check(movingBackgroundAccepted, "moving game backgrounds do not hide a fixed task glyph");
    check(!evidence.Observe(synthetic(61, false), roi, false, true, false).visible,
        "removed HUD icon is hidden on the first missing frame without a time grace period");
    for (int shift = 1; shift <= 8; ++shift) evidence.Observe(synthetic(shift, true, shift), roi, false, true, false);
    check(!evidence.Observe(synthetic(9, true, 8), roi, false, true, false).visible &&
        evidence.Observe(first, roi, false, true, false).visible,
        "weak fallback cannot walk its cached glyph onto a drifting background pattern");
    check(!evidence.Observe(first, roi, false, false, false).visible &&
        !evidence.Observe(first, roi, false, true, false).visible,
        "focus loss clears the HUD template until another independent detection");
    evidence.Observe(first, roi, true, true, false);
    check(!evidence.Observe(first, roi, false, true, true).visible &&
        !evidence.Observe(first, roi, false, true, false).visible,
        "confirmed big map invalidates the gameplay template immediately");
    evidence.Observe(first, roi, true, true, false);
    cv::Mat resized;
    cv::resize(first, resized, {640, 360});
    check(!evidence.Observe(resized, cv::Rect(24, 48, 108, 96), false, true, false).visible,
        "capture resize invalidates cached HUD geometry");
    evidence.Observe(first, roi, true, true, false);
    check(!evidence.Observe(first, cv::Rect(-2, 0, 54, 48), false, true, false).visible &&
        !evidence.Observe(first, roi, false, true, false).visible,
        "invalid crop safely clears the HUD template");
}
