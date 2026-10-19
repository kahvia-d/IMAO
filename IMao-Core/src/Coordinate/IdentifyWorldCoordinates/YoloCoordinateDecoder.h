#pragma once
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <vector>

namespace CoordinateYolo {
using Json = nlohmann::json;
// Frozen own-model evidence pipeline. Inputs are detections in normalized ROI pixels.
Json Decode(const Json& detections);
Json Select(const Json& raw, const Json& contrast, const cv::Mat& contrastImage, bool calibrated);
}
