#pragma once
#include <opencv2/opencv.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>
#include "CoordinatePreprocessing.generated.h"
#include "CoordinateAspectPreprocessing.generated.h"

// Experiments only. The first three variants reuse the extracted production body
// with its horizontal cap removed; the fourth preserves unthresholded BGR pixels.
inline std::vector<cv::Mat> PreprocessOfflineAspect(const cv::Mat& source) {
    if (source.empty() || source.type() != CV_8UC3)
        throw std::runtime_error("aspect preprocessing requires BGR pixels");
    const int width = std::max(1, static_cast<int>(std::lround(source.cols * 48.0 / source.rows)));
    if (width > 3192) throw std::runtime_error("aspect input exceeds tested model width; no silent squeezing");
    auto variants = PreprocessCoordinateRegionAspect(source);
    cv::Mat raw;
    cv::resize(source, raw, cv::Size(width, 48), 0, 0, cv::INTER_CUBIC);
    cv::copyMakeBorder(raw, raw, 0, 0, 4, 4, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
    variants.push_back(std::move(raw));
    return variants;
}
