#pragma once

#include <opencv2/core.hpp>
#include <string>

// Development-only capture diagnostics.  The functions become no-ops unless
// IMAO_ENABLE_DIAGNOSTICS is defined by the CMake preset.
namespace Diagnostics {
    void Initialize();
    bool Enabled();
    void SetCaptureEnabled(bool enabled);
    void Record(const std::string& eventName, const std::string& details);
    void SaveImage(const std::string& tag, const cv::Mat& image);
    std::string SessionDirectory();
}
