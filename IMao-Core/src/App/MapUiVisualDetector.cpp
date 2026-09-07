#include "MapUiVisualDetector.h"

#include <algorithm>
#include <cmath>
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

bool MapUiVisualDetector::DetectBigMapControls(const cv::Mat& snapshot, const RECT& clientRect) {
    if (snapshot.empty() || (snapshot.channels() != 3 && snapshot.channels() != 4) ||
        snapshot.depth() != CV_8U || snapshot.cols != clientRect.right - clientRect.left ||
        snapshot.rows != clientRect.bottom - clientRect.top) return false;

    // Normalize only the narrow zoom-control strip, keeping this probe cheap
    // enough to run on the state thread. The two circular +/- buttons are
    // fixed UI, independent of map texture, panning, zoom, and player position.
    const cv::Rect strip(cvRound(snapshot.cols * 1480.0 / kReferenceWidth),
        cvRound(snapshot.rows * 235.0 / kReferenceHeight),
        cvRound(snapshot.cols * 60.0 / kReferenceWidth),
        cvRound(snapshot.rows * 410.0 / kReferenceHeight));
    if ((strip & cv::Rect(0, 0, snapshot.cols, snapshot.rows)) != strip) return false;
    cv::Mat normalized;
    cv::resize(snapshot(strip), normalized, {60, 410}, 0, 0, cv::INTER_AREA);
    cv::Mat bgr;
    if (normalized.channels() == 4) cv::cvtColor(normalized, bgr, cv::COLOR_BGRA2BGR);
    else bgr = normalized;
    cv::Mat white(bgr.size(), CV_8UC1, cv::Scalar(0));
    cv::Mat pale(bgr.size(), CV_8UC1, cv::Scalar(0));
    for (int y = 0; y < bgr.rows; ++y) {
        for (int x = 0; x < bgr.cols; ++x) {
            const auto pixel = bgr.at<cv::Vec3b>(y, x);
            const int low = std::min({pixel[0], pixel[1], pixel[2]});
            const int high = std::max({pixel[0], pixel[1], pixel[2]});
            white.at<uchar>(y, x) = low >= 155 && high - low <= 65 ? 255 : 0;
            // The thin ring is translucent, unlike the bright +/- glyph.
            pale.at<uchar>(y, x) = low >= 90 && high - low <= 65 ? 255 : 0;
        }
    }
    const auto brightNear = [&white](int x, int y) {
        return cv::countNonZero(white(cv::Rect(x - 1, y - 1, 3, 3))) > 0;
    };
    const auto button = [&](int expectedY, bool plus, int& foundX) {
        for (int y = expectedY - 6; y <= expectedY + 6; ++y) {
            for (int x = 23; x <= 35; ++x) {
                int horizontal = 0, vertical = 0, ring = 0, darkCorners = 0;
                for (int d = -5; d <= 5; ++d) {
                    horizontal += brightNear(x + d, y);
                    if (std::abs(d) >= 3) vertical += brightNear(x, y + d);
                }
                if (horizontal < 10 || (plus ? vertical < 5 : vertical > 2)) continue;
                for (int sector = 0; sector < 16; ++sector) {
                    const double angle = sector * CV_PI / 8;
                    const int ringX = x + cvRound(10 * std::cos(angle));
                    const int ringY = y + cvRound(10 * std::sin(angle));
                    ring += cv::countNonZero(pale(cv::Rect(ringX - 1, ringY - 1, 3, 3))) > 0;
                }
                for (int dx : {-4, 4}) for (int dy : {-4, 4}) {
                    darkCorners += !brightNear(x + dx, y + dy);
                }
                if (ring >= 12 && darkCorners >= 3) {
                    foundX = x;
                    return true;
                }
            }
        }
        return false;
    };
    int plusX = 0, minusX = 0;
    return button(30, true, plusX) && button(380, false, minusX) && std::abs(plusX - minusX) <= 3;
}
