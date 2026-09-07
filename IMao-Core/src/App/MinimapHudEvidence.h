#pragma once

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <vector>

struct MinimapHudObservation {
    bool visible = false;
    bool usedTemplate = false;
    bool templateReady = false;
    double foregroundRecall = 0.0;
    double shapeAgreement = 0.0;
};

// The task glyph is a fixed, opaque HUD shape over a changing game background.
// SURF positively identifies it, but a missed descriptor is not evidence that
// it vanished. Remember only its bright strokes and enclosed dark details,
// then require those same details in the current frame when SURF is sparse.
class MinimapHudEvidence {
public:
    void Reset() {
        foreground_.release(); darkDetails_.release();
        snapshotSize_ = {}; iconRoi_ = {};
    }

    MinimapHudObservation Observe(const cv::Mat& snapshot, const cv::Rect& iconRoi,
        bool strongSurfEvidence, bool focused, bool definiteBigMap) {
        MinimapHudObservation result;
        if (!focused || definiteBigMap || snapshot.empty() || snapshot.depth() != CV_8U ||
            (snapshot.channels() != 3 && snapshot.channels() != 4) || iconRoi.empty() ||
            (iconRoi & cv::Rect(0, 0, snapshot.cols, snapshot.rows)) != iconRoi) {
            Reset(); return result;
        }
        if (snapshot.size() != snapshotSize_ || iconRoi != iconRoi_) {
            Reset(); snapshotSize_ = snapshot.size(); iconRoi_ = iconRoi;
        }
        cv::Mat normalized, bgr, gray;
        cv::resize(snapshot(iconRoi), normalized, {54, 48}, 0, 0, cv::INTER_LINEAR);
        if (normalized.channels() == 4) cv::cvtColor(normalized, bgr, cv::COLOR_BGRA2BGR);
        else bgr = normalized;
        cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
        cv::Mat bright = cv::Mat::zeros(gray.size(), CV_8UC1);
        for (int y = 0; y < bgr.rows; ++y) {
            for (int x = 0; x < bgr.cols; ++x) {
                const auto pixel = bgr.at<cv::Vec3b>(y, x);
                const int low = std::min({pixel[0], pixel[1], pixel[2]});
                const int high = std::max({pixel[0], pixel[1], pixel[2]});
                bright.at<unsigned char>(y, x) = low >= 155 && high - low <= 65 ? 255 : 0;
            }
        }
        if (strongSurfEvidence) {
            // Only a fresh independent SURF detection may replace the template.
            // Fallback successes never become their own reference.
            Learn(bright, gray);
            result.visible = true;
            result.templateReady = !foreground_.empty();
            return result;
        }
        result.templateReady = !foreground_.empty();
        if (!result.templateReady) return result;

        cv::Mat presentForeground, presentDark;
        cv::bitwise_and(bright, foreground_, presentForeground);
        cv::bitwise_and(gray < 140, darkDetails_, presentDark);
        result.foregroundRecall = static_cast<double>(cv::countNonZero(presentForeground)) /
            cv::countNonZero(foreground_);
        const double darkRecall = static_cast<double>(cv::countNonZero(presentDark)) /
            cv::countNonZero(darkDetails_);
        result.shapeAgreement = std::min(result.foregroundRecall, darkRecall);
        result.visible = result.foregroundRecall >= 0.82 && darkRecall >= 0.78;
        result.usedTemplate = result.visible;
        return result;
    }

private:
    void Learn(const cv::Mat& bright, const cv::Mat& gray) {
        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(bright, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
        if (contours.empty()) return;
        const auto largest = std::max_element(contours.begin(), contours.end(),
            [](const auto& left, const auto& right) { return cv::contourArea(left) < cv::contourArea(right); });
        if (cv::contourArea(*largest) < bright.total() * 0.08) return;
        std::vector<cv::Point> hull;
        cv::convexHull(*largest, hull);
        cv::Mat inside = cv::Mat::zeros(bright.size(), CV_8UC1);
        cv::fillConvexPoly(inside, hull, cv::Scalar(255));
        cv::Mat foreground, darkDetails;
        cv::bitwise_and(inside, bright, foreground);
        cv::bitwise_and(inside, gray < 100, darkDetails);
        const int brightCount = cv::countNonZero(foreground);
        const int darkCount = cv::countNonZero(darkDetails);
        // A blank bright patch has no internal shape; a dim background has no
        // strong glyph. Neither may prime the weak-evidence path.
        if (brightCount < 80 || brightCount > bright.total() * 0.82 || darkCount < 20) return;
        foreground_ = foreground;
        darkDetails_ = darkDetails;
    }

    cv::Size snapshotSize_;
    cv::Rect iconRoi_;
    cv::Mat foreground_, darkDetails_;
};
