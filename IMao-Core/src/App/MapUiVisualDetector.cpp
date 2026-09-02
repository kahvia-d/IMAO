#include "MapUiVisualDetector.h"

#include <algorithm>
#include <opencv2/imgproc.hpp>

namespace {
constexpr double kReferenceWidth = 1600.0;
constexpr double kReferenceHeight = 900.0;

cv::Rect ScaleCrop(const cv::Mat& snapshot, const RECT& clientRect) {
    const double width = clientRect.right - clientRect.left;
    const double height = clientRect.bottom - clientRect.top;
    if (snapshot.empty() || width <= 0.0 || height <= 0.0) return {};

    // 10, 52 to 82, 116 on the 1600x900 reference UI.  The slightly larger
    // box admits the complete compass but excludes the gameplay player's
    // yellow minimap arrow at the right edge.
    const int left = static_cast<int>(10.0 * width / kReferenceWidth);
    const int top = static_cast<int>(52.0 * height / kReferenceHeight);
    const int right = static_cast<int>(82.0 * width / kReferenceWidth);
    const int bottom = static_cast<int>(116.0 * height / kReferenceHeight);
    const cv::Rect requested(left, top, std::max(0, right - left), std::max(0, bottom - top));
    return requested & cv::Rect(0, 0, snapshot.cols, snapshot.rows);
}
}

MapCompassDetection MapUiVisualDetector::DetectBigMapCompass(const cv::Mat& snapshot, const RECT& clientRect) {
    MapCompassDetection result;
    const cv::Rect crop = ScaleCrop(snapshot, clientRect);
    if (crop.empty() || snapshot.channels() < 3) return result;

    cv::Mat bgr;
    if (snapshot.channels() == 3) bgr = snapshot;
    else if (snapshot.channels() == 4) cv::cvtColor(snapshot, bgr, cv::COLOR_BGRA2BGR);
    else return result;
    const cv::Mat compass = bgr(crop);
    result.cropPixels = compass.rows * compass.cols;
    for (int row = 0; row < compass.rows; ++row) {
        for (int column = 0; column < compass.cols; ++column) {
            const cv::Vec3b pixel = compass.at<cv::Vec3b>(row, column);
            const int blue = pixel[0];
            const int green = pixel[1];
            const int red = pixel[2];
            if (red > 120 && green > 90 && blue < 100 && red - blue > 50) {
                ++result.goldPixels;
            }
        }
    }

    // The reference map has roughly 8% gold pixels in this crop; gameplay
    // samples have none.  Three percent keeps room for capture scaling and
    // anti-aliasing while remaining well below the observed map signature.
    result.visible = result.cropPixels > 0 && result.goldPixels * 100 >= result.cropPixels * 3;
    return result;
}
