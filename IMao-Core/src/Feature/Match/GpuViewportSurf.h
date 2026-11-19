#pragma once
#include <opencv2/core.hpp>
#include <memory>
#include <string>
#include <vector>

// Offsets relative to a sample in the integral image, followed by its Haar weight.
struct ViewportHaarFilter {
    int p0 = 0, p1 = 0, p2 = 0, p3 = 0;
    float w = 0;
};
struct ViewportSurfPeak {
    int x, y, layer, trace;
    float neighbourhood[3][9];
};

class GpuViewportSurf {
public:
    GpuViewportSurf();
    ~GpuViewportSurf();
    bool BeginImage(const cv::Mat& integral);
    bool FindMaxima(int octave, const ViewportHaarFilter (&filters)[5][10],
        std::vector<ViewportSurfPeak>& peaks);
    const std::string& Failure() const { return failure_; }
private:
    struct Device;
    std::unique_ptr<Device> device_;
    std::string failure_;
};
