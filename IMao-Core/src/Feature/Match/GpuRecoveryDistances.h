#pragma once
#include <memory>
#include <string>
#include <opencv2/core/mat.hpp>

// Owned by the global-search worker. Local tracking never submits work here.
class GpuRecoveryDistances {
public:
    GpuRecoveryDistances();
    ~GpuRecoveryDistances();
    bool Compute(const cv::Mat& map, const cv::Mat& query, cv::Mat& distances);
    const std::string& Failure() const { return failure_; }
private:
    struct Device;
    std::unique_ptr<Device> device_;
    std::string failure_;
};
