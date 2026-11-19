#pragma once
#include "Coordinate/VisualLocalization/GlobalVisualLocalizer.h"
#include "Feature/Match/EncodedExactMatcher.h"
#include "Feature/Match/GpuRecoveryDistances.h"
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

// Explicit hardware gate for the experimental backend, including edge lanes,
// tied neighbours, partial batches, uint8 decode and CPU fallback.
inline int TestGpuRecoveryDistances() {
    GpuRecoveryDistances gpu;
    cv::RNG rng(451);
    cv::Mat query(129,128,CV_32F);
    rng.fill(query, cv::RNG::UNIFORM, -0.15f, 0.15f);
    query.row(0).copyTo(query.row(1));
    ImageFeatureData map;
    map.imgDescriptors.create(4113,128,CV_8U);
    rng.fill(map.imgDescriptors, cv::RNG::UNIFORM, 0, 256);
    map.descriptorEncoding = {{2097, 0.001f, -128}, {4113, 0.002f, -110}};
    map.imgDescriptors.row(0).copyTo(map.imgDescriptors.row(1));
    map.CopyDescriptorRowTo(0, query.ptr<float>(0));
    query.row(0).copyTo(query.row(1));
    using Matches = std::vector<std::vector<cv::DMatch>>;
    Matches expected, expectedReverse;
    MatchDescriptorsExactly(map, query, expected, expectedReverse);
    const auto equal = [](const Matches& a, const Matches& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (a[i].size() != b[i].size()) return false;
            for (size_t j = 0; j < a[i].size(); ++j) {
                const auto& x = a[i][j];
                const auto& y = b[i][j];
                if (x.trainIdx != y.trainIdx || x.queryIdx != y.queryIdx ||
                    !(std::abs(x.distance-y.distance) < 2e-5f)) return false;
            }
        }
        return true;
    };
    for (bool fallback : {false, true}) {
        int batches = 0;
        Matches forward, reverse;
        MatchDescriptorsExactly(map, query, forward, reverse, {}, [&](const cv::Mat& rows,
                const cv::Mat& frame, cv::Mat& distances) {
            ++batches;
            if (fallback) return false;
            if (!gpu.Compute(rows, frame, distances)) {
                throw std::runtime_error("hardware recovery test: " + gpu.Failure());
            }
            return true;
        });
        if (batches < 2 || !equal(forward, expected) || !equal(reverse, expectedReverse)) return 1;
    }
    std::cout << "GPU and fallback nearest neighbours match CPU in both directions\n";
    return 0;
}
