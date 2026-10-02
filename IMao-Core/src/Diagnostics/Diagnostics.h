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
    // One line per named milestone: working set, private bytes and peak, written to the structured log in
    // every build. A player reporting memory used to be unanswerable - the process's 1.5 GB could only be
    // attributed by subtracting headless measurements from a live one, which reads as OCR or as the packs
    // depending on who is guessing (2026-10-02). These lines make the attribution the process's own.
    void RecordMemory(const std::string& point);
    void SaveImage(const std::string& tag, const cv::Mat& image);
    std::string SessionDirectory();
}
