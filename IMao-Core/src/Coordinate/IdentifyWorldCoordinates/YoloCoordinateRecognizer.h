#pragma once
#include "YoloCoordinateDecoder.h"
#include <filesystem>
#include <memory>

class YoloCoordinateRecognizer {
public:
    explicit YoloCoordinateRecognizer(const std::filesystem::path& directory);
    ~YoloCoordinateRecognizer();
    YoloCoordinateRecognizer(const YoloCoordinateRecognizer&) = delete;
    YoloCoordinateRecognizer& operator=(const YoloCoordinateRecognizer&) = delete;
    CoordinateYolo::Json Detect(const cv::Mat& normalizedBgr);
    CoordinateYolo::Json Recognize(const cv::Mat& crop, bool calibrated);
    CoordinateYolo::Json RuntimeInfo() const;
    static cv::Rect Region(cv::Size frame);
    static std::pair<cv::Mat,cv::Mat> Prepare(const cv::Mat& crop);
private:
    struct State;
    std::unique_ptr<State> state_;
    std::filesystem::path directory_;
    bool directMl_ = false;
    std::string requested_ = "cpu", fallback_;
    int adapter_ = 0;
    void Initialize(bool directMl);
    CoordinateYolo::Json DetectImpl(const cv::Mat& normalizedBgr);
};
