#pragma once
#include "Coordinate/VisualLocalization/GlobalVisualLocalizer.h"
#include <opencv2/xfeatures2d.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <filesystem>
#include <iostream>

inline int TestMinimapPreparation(const std::filesystem::path& directory) {
    int checked = 0, changedMasks = 0;
    for (const auto& file : std::filesystem::directory_iterator(directory)) {
        if (file.path().extension() != ".png") continue;
        const auto frame = cv::imread(file.path().string());
        if (frame.empty()) return 1;
        cv::Mat previous, originalMask;
        cv::resize(frame, previous, {184,184}, 0, 0, cv::INTER_AREA);
        for (int variant = 0; variant < 3; ++variant) {
            if (variant == 2) cv::rectangle(previous, {87,22,14,19}, {255,255,255}, cv::FILLED);
            cv::Mat normalized, gray, descriptors;
            ImageFeatureData actual;
            MinimapFeatureDiagnostics diagnostics;
            if (!GlobalVisualLocalizer::PrepareMinimap(frame, normalized, actual,
                variant == 0 ? nullptr : &previous, &diagnostics)) return 1;
            if (variant == 0) originalMask = diagnostics.featureMask;
            else if (cv::norm(originalMask, diagnostics.featureMask, cv::NORM_INF) != 0) ++changedMasks;
            cv::cvtColor(normalized, gray, cv::COLOR_BGR2GRAY);
            std::vector<cv::KeyPoint> points;
            cv::xfeatures2d::SURF::create(60,8,4,true,true)->detectAndCompute(
                gray, diagnostics.featureMask, points, descriptors);
            if (points.size() != actual.imgKeypoints.size() ||
                cv::norm(descriptors, actual.imgDescriptors, cv::NORM_INF) != 0) return 1;
            for (size_t i = 0; i < points.size(); ++i) {
                const auto& a = points[i];
                const auto& b = actual.imgKeypoints[i];
                if (a.pt != b.pt || a.size != b.size || a.angle != b.angle || a.response != b.response ||
                    a.octave != b.octave || a.class_id != b.class_id) return 1;
            }
            ++checked;
        }
    }
    std::cout << "SURF equivalence cases=" << checked << " changedMasks=" << changedMasks << '\n';
    return checked > 0 && changedMasks > 0 ? 0 : 1;
}
