#pragma once
#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>
#include <functional>
#include <cstdint>

struct ViewportSurfStatistics {
    // Integral image + live Hessian determinant/trace matrices only.
    // Excludes caller pixels, descriptors, keypoints and library/parallel overhead.
    size_t detectorScratchBytes = 0;
    double detectMilliseconds = 0;
    double describeMilliseconds = 0;
    unsigned descriptorWorkers = 0; // Dispatch bound, including the calling worker.
    uint64_t descriptorWindowPixels = 0;
    int descriptorMaxWindow = 0;
};

// Same CPU SURF(100,4,3,true,true), with at most three adjacent Hessian layers.
// No mask, scale omission, feature pruning or changed descriptor arithmetic.
// Input is the same grayscale conversion used by FeatureMatch. On cancellation,
// no partial output escapes; all detector temporaries are owned by this call.
// Descriptor callbacks are serialized between 32-keypoint chunks; descriptor
// execution uses at most four participants independently of OpenCV's guard.
void ExtractViewportSurf(const cv::Mat& gray, std::vector<cv::KeyPoint>& points,
    cv::Mat& descriptors, const std::function<bool()>& interrupted = {},
    ViewportSurfStatistics* statistics = nullptr);
