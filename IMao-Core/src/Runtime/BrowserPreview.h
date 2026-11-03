#pragma once
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <stdexcept>
#include <vector>

namespace BrowserPreview {
// The managed IPC reader accepts at most 1 MiB per JSON line. Base64 expands
// binary data by 4/3, so leave ample space for the response envelope as well.
inline constexpr std::size_t MaximumPngBytes = 512 * 1024;
inline std::vector<unsigned char> Encode(const cv::Mat& image) {
    if (image.empty() || image.depth() != CV_8U || (image.channels() != 3 && image.channels() != 4))
        throw std::runtime_error("窗口预览图像格式无效");
    const double scale = (std::min)(1.0, 960.0 / (std::max)(image.cols, image.rows));
    cv::Mat preview;
    cv::resize(image, preview, {}, scale, scale, cv::INTER_AREA);
    if (preview.channels() == 4) cv::cvtColor(preview, preview, cv::COLOR_BGRA2BGR);
    for (;;) {
        std::vector<unsigned char> png;
        if (!cv::imencode(".png", preview, png)) throw std::runtime_error("窗口预览编码失败");
        if (png.size() <= MaximumPngBytes) return png;
        const cv::Size next((std::max)(1, preview.cols * 3 / 4), (std::max)(1, preview.rows * 3 / 4));
        if (next == preview.size()) throw std::runtime_error("窗口预览超过消息大小限制");
        cv::resize(preview, preview, next, 0, 0, cv::INTER_AREA);
    }
}
}
