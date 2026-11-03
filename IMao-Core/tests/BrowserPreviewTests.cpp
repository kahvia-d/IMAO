#include "Runtime/BrowserPreview.h"
#include <iostream>

int main() {
    for (const auto size : {cv::Size(1920,1080), cv::Size(3840,2160), cv::Size(640,12000)}) {
        cv::Mat noise(size, CV_8UC4);
        cv::RNG random(12345); random.fill(noise, cv::RNG::UNIFORM, 0, 256);
        const auto png = BrowserPreview::Encode(noise);
        const auto decoded = cv::imdecode(png, cv::IMREAD_COLOR);
        // Include a larger-than-normal envelope, not only the raw image bytes.
        const auto messageSize = ((png.size()+2)/3)*4 + 4096;
        if (messageSize >= 1024*1024 || decoded.empty() || decoded.cols > 960 || decoded.rows > 960) {
            std::cerr << "FAIL preview must decode and fit managed IPC\n"; return 1;
        }
        std::cout << "PASS " << size << " pngBytes=" << png.size() << " messageBudget=" << messageSize << '\n';
    }
    try { BrowserPreview::Encode({}); return 1; } catch (const std::runtime_error&) {}
    return 0;
}
