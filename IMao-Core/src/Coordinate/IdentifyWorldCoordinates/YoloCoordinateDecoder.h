#pragma once
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <vector>
#include <optional>

namespace CoordinateYolo {
using Json = nlohmann::json;
// Frozen own-model evidence pipeline. Inputs are detections in normalized ROI pixels.
Json Decode(const Json& detections);
bool CanSelectRaw(const Json& detections);
struct DigitDispute {
    std::string weakerDigit;
    double center, width, competingScore;
};
std::vector<DigitDispute> FindDigitDisputes(const Json& raw, const Json& contrast);
struct SamplingContext {
    double translation;
    bool calibrated;
    bool checkMargin = true;
    double horizontalGain = 1;
    const std::vector<DigitDispute>* disputes = nullptr;
};
Json Select(const Json& raw, const Json& contrast, const cv::Mat& contrastImage, bool calibrated,
            std::optional<SamplingContext> sampling = std::nullopt);
bool HasDigitAtLeftBoundary(const Json& raw, const Json& contrast);
bool HasNominalTextSpan(const Json& detections, int contentWidth);
}
