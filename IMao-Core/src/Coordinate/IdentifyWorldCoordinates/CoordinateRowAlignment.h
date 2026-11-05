#pragma once
#include <filesystem>

#include <vector>
#include <opencv2/core.hpp>

namespace CoordinateYolo {
struct RowFit {
    double top = 0;
    double height = 0;
    double score = -1;
};

class RowAlignment {
public:
    explicit RowAlignment(const std::filesystem::path& modelDirectory);
    bool Enabled() const { return !templates_.empty(); }
    RowFit Fit(const cv::Mat& crop, bool hasDetections, const RowFit* hint) const;
    static cv::Mat Resample(const cv::Mat& crop, const RowFit& fit, cv::Point2d phase);
private:
    std::vector<cv::Mat> templates_, brightTemplates_;
};
}
