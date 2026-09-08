#include "GamepadMapCursorDetector.h"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace {
struct Ring { cv::Point2d center; double radius, score; };
int Pale(const cv::Mat& image, const cv::Point2d& point) {
    const int x = cvRound(point.x), y = cvRound(point.y);
    if (x < 0 || y < 0 || x >= image.cols || y >= image.rows) return 0;
    const auto p = image.at<cv::Vec3b>(y, x);
    const int low = std::min({p[0], p[1], p[2]}), high = std::max({p[0], p[1], p[2]});
    return high - low <= 48 ? low : 0;
}
double VerifyRing(const cv::Mat& image, cv::Point2d center, double radius) {
    constexpr int samples = 96;
    static const auto directions = [] {
        std::array<cv::Point2d, samples> values;
        for (int i = 0; i < samples; ++i) {
            const double angle = i * 2.0 * CV_PI / samples;
            values[i] = {std::cos(angle), std::sin(angle)};
        }
        return values;
    }();
    int white = 0, contrast = 0, run = 0, maximumGap = 0, initialGap = 0;
    double brightness = 0.0;
    // Angular coverage and both dark shoulders reject thick/filled symbols,
    // arcs, square/diamond map icons, and white text whose bounds look round.
    for (int index = 0; index < samples; ++index) {
        const auto direction = directions[index];
        int ring = 0;
        for (double offset : {-0.8, 0.0, 0.8}) ring = std::max(ring, Pale(image, center + direction * (radius + offset)));
        const bool hit = ring >= 195;
        if (hit) run = 0;
        else {
            maximumGap = std::max(maximumGap, ++run);
            if (index == initialGap) ++initialGap;
        }
        white += hit;
        if (index + 1 - white > 6 || maximumGap > 5) return 0.0;
        brightness += Pale(image, center + direction * radius);
        const int inside = Pale(image, center + direction * (radius - 4.0));
        const int outside = Pale(image, center + direction * (radius + 4.0));
        contrast += hit && ring - inside >= 35 && ring - outside >= 35;
    }
    maximumGap = std::max(maximumGap, run + initialGap);
    const double coverage = white / static_cast<double>(samples);
    const double shoulders = contrast / static_cast<double>(samples);
    if (coverage < 0.93 || shoulders < 0.82 || maximumGap > 5 || brightness / samples < 215) return 0.0;
    int filled = 0, innerPixels = 0;
    const int inner = cvRound(radius * 0.70);
    for (int y = -inner; y <= inner; ++y) for (int x = -inner; x <= inner; ++x) {
        if (x * x + y * y > inner * inner) continue;
        ++innerPixels; filled += Pale(image, center + cv::Point2d(x, y)) >= 195;
    }
    if (filled > innerPixels * 0.30) return 0.0;
    return 0.50 * coverage + 0.30 * shoulders + 0.20 * brightness / (samples * 255.0);
}
}

GamepadMapCursorDetection GamepadMapCursorDetector::Detect(const cv::Mat& snapshot, const RECT& clientRect) {
    GamepadMapCursorDetection result;
    if (snapshot.empty() || snapshot.depth() != CV_8U || (snapshot.channels() != 3 && snapshot.channels() != 4) ||
        snapshot.cols != clientRect.right - clientRect.left || snapshot.rows != clientRect.bottom - clientRect.top ||
        snapshot.cols < 640 || snapshot.rows < 360) return result;
    const auto controls = MapUiVisualDetector::DetectBigMapControlLayout(snapshot, clientRect);
    // The assistant can cover the compass. Require the complete independent
    // controller zoom strip, while leaving map-state gating to its caller.
    if (controls.mouse || controls.controllerTriggerAnchors != 2 || !controls.controllerSlider) return result;

    const double scale = 900.0 / snapshot.rows;
    cv::Mat normalized, bgr;
    cv::resize(snapshot, normalized, {cvRound(snapshot.cols * scale), 900}, 0, 0, cv::INTER_AREA);
    if (normalized.channels() == 4) cv::cvtColor(normalized, bgr, cv::COLOR_BGRA2BGR); else bgr = normalized;
    // Exclude the fixed command bars and zoom strip, not an assumed cursor
    // center. The complete remaining map body is searched for visual evidence.
    const cv::Rect body(cvRound(bgr.cols * 0.04), 125, cvRound(bgr.cols * 0.86), 650);
    cv::Mat white(body.size(), CV_8UC1, cv::Scalar(0));
    for (int y = 0; y < body.height; ++y) {
        const auto* input = bgr.ptr<cv::Vec3b>(body.y + y) + body.x;
        auto* output = white.ptr<uchar>(y);
        for (int x = 0; x < body.width; ++x) {
            const int low = std::min({input[x][0], input[x][1], input[x][2]});
            const int high = std::max({input[x][0], input[x][1], input[x][2]});
            output[x] = low >= 195 && high - low <= 48 ? 255 : 0;
        }
    }
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(white, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);
    std::vector<Ring> accepted;
    for (const auto& contour : contours) {
        if (contour.size() < 20) continue;
        const auto bounds = cv::boundingRect(contour);
        if (bounds.width < 36 || bounds.height < 36 || bounds.width > 76 || bounds.height > 76) continue;
        const auto ellipse = cv::fitEllipse(contour);
        const double shortAxis = std::min(ellipse.size.width, ellipse.size.height), longAxis = std::max(ellipse.size.width, ellipse.size.height);
        if (shortAxis < 34 || longAxis / shortAxis > 1.10) continue;
        const double approximateRadius = (shortAxis + longAxis) / 4.0;
        if (std::abs(cv::contourArea(contour)) < CV_PI * approximateRadius * approximateRadius * 0.80) continue;
        const cv::Point2d center(ellipse.center.x + body.x, ellipse.center.y + body.y);
        Ring candidate{center, 0, 0};
        for (double radius = approximateRadius - 2.5; radius <= approximateRadius + 2.5; radius += 0.5) {
            const double score = VerifyRing(bgr, center, radius);
            if (score > candidate.score) candidate = {center, radius, score};
        }
        if (candidate.score == 0) continue;
        bool duplicate = false;
        for (auto& previous : accepted) if (cv::norm(previous.center - center) < 4.0) {
            if (candidate.score > previous.score) previous = candidate;
            duplicate = true; break;
        }
        if (!duplicate) accepted.push_back(candidate);
    }
    // Multiple distinct qualifying rings cannot identify a unique game cursor.
    if (accepted.size() != 1) return result;
    result.visible = true;
    result.center = accepted.front().center / scale;
    result.radius = accepted.front().radius / scale;
    result.confidence = accepted.front().score;
    return result;
}
