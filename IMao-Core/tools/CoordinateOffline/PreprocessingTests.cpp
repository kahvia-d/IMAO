#include <iostream>
#include "OfflinePreprocessing.h"
#include <include/preprocess_op.h>
int main() {
    try {
        cv::Mat image(20, 240, CV_8UC3, cv::Scalar(0, 0, 0));
        image(cv::Rect(6, 8, 10, 2)).setTo(cv::Scalar(128, 128, 128));
        const auto variants = PreprocessOfflineAspect(image);
        if (variants.size() != 4) throw std::runtime_error("contrast,binary,tophat,raw routes required");
        for (const auto& variant : variants)
            if (variant.size() != cv::Size(584, 48)) throw std::runtime_error("aspect ratio was squeezed");
        PaddleOCR::CrnnResizeImg resize;
        cv::Mat modelResize;
        resize.Run(variants[3], modelResize, variants[3].cols / 48.0f, false, {3, 48, 320});
        if (modelResize.cols < 584 || modelResize.rows != 48)
            throw std::runtime_error("linked CRNN resize reintroduced horizontal squeeze");
        if (!cv::countNonZero(variants[3](cv::Rect(18, 19, 24, 5)).reshape(1)))
            throw std::runtime_error("leading minus disappeared from raw route");
        for (const auto& variant : PreprocessCoordinateRegion(image))
            if (variant.size() != cv::Size(320, 48)) throw std::runtime_error("production preprocessing changed");
        bool rejected = false;
        try { PreprocessOfflineAspect(cv::Mat(1, 100, CV_8UC3)); }
        catch (const std::runtime_error&) { rejected = true; }
        if (!rejected) throw std::runtime_error("oversized aspect input must fail explicitly");
        std::cout << "Offline preprocessing contracts passed\n";
        std::cout << "Linked CRNN resize: " << modelResize.cols << 'x' << modelResize.rows << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
