#pragma once

#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace MinimapTerrainEvidence {
// Normalized minimaps are 184 square. Keep the fixed mask for feature-rich
// images; this mask recovers terrain near the arrow in sparse images only.
inline cv::Mat TerrainMask(const cv::Mat& image) {
    cv::Mat mask(image.size(), CV_8U, cv::Scalar(0));
    const cv::Point center(image.cols / 2, image.rows / 2);
    cv::circle(mask, center, 80, cv::Scalar(255), cv::FILLED);
    if (image.channels() < 3) {
        cv::circle(mask, center, 42, cv::Scalar(0), cv::FILLED);
        return mask;
    }
    cv::Mat bgr, hsv, colored;
    if (image.channels() == 4) cv::cvtColor(image, bgr, cv::COLOR_BGRA2BGR);
    else bgr = image;
    cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);
    // Bright saturated HUD symbols, including the yellow arrow, cannot be
    // static terrain evidence. Dilate to exclude their anti-aliased borders.
    cv::inRange(hsv, cv::Scalar(0, 100, 140), cv::Scalar(180, 255, 255), colored);
    cv::dilate(colored, colored, cv::getStructuringElement(cv::MORPH_ELLIPSE, {11, 11}));
    mask.setTo(0, colored);
    cv::circle(mask, center, 9, cv::Scalar(0), cv::FILLED);
    return mask;
}

// Locate a unique bright yellow player arrow. Input is a map crop resized to
// the nominal 1600x900 UI scale. Ambiguous yellow symbols supply no hint.
inline bool PlayerArrow(const cv::Mat& image, cv::Point2f& position) {
    if (image.empty() || image.channels() < 3) return false;
    cv::Mat bgr, hsv, yellow;
    if (image.channels() == 4) cv::cvtColor(image, bgr, cv::COLOR_BGRA2BGR);
    else bgr = image;
    cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);
    cv::inRange(hsv, cv::Scalar(18, 120, 210), cv::Scalar(40, 255, 255), yellow);
    cv::morphologyEx(yellow, yellow, cv::MORPH_CLOSE,
        cv::getStructuringElement(cv::MORPH_ELLIPSE, {3, 3}));
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(yellow, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    std::vector<cv::Point2f> candidates;
    for (const auto& contour : contours) {
        const double area = cv::contourArea(contour);
        const auto bounds = cv::boundingRect(contour);
        if (area < 90 || area > 1500 || bounds.width < 12 || bounds.height < 12 ||
            bounds.width > 65 || bounds.height > 65 || bounds.x <= 1 || bounds.y <= 1 ||
            bounds.br().x >= image.cols - 1 || bounds.br().y >= image.rows - 1) continue;
        const double aspect = static_cast<double>(bounds.width) / bounds.height;
        std::vector<cv::Point> hull, polygon;
        cv::convexHull(contour, hull);
        cv::approxPolyDP(hull, polygon, cv::arcLength(hull, true) * 0.055, true);
        const double solidity = area / std::max(1.0, cv::contourArea(hull));
        // The white centre and open notch make the yellow contour a chevron;
        // its convex hull, rather than its filled area, carries the triangle.
        if (aspect < 0.45 || aspect > 2.2 || polygon.size() != 3 || solidity < 0.35) continue;
        // The anchor is the centre of the triangle, not its heading tip.
        const auto moments = cv::moments(hull);
        candidates.emplace_back(static_cast<float>(moments.m10 / moments.m00),
            static_cast<float>(moments.m01 / moments.m00));
    }
    if (candidates.size() != 1) return false;
    position = candidates.front();
    return true;
}

struct Motion {
    cv::Point2d shift;
    double score = 0.0;
    double separation = 0.0;
    int support = 0;
};

inline void Gradients(const cv::Mat& image, cv::Mat& gx, cv::Mat& gy, cv::Mat& mask, bool crossView = false) {
    cv::Mat gray;
    if (image.channels() == 1) gray = image.clone();
    else cv::cvtColor(image, gray, image.channels() == 4 ? cv::COLOR_BGRA2GRAY : cv::COLOR_BGR2GRAY);
    cv::GaussianBlur(gray, gray, {3, 3}, 0.7);
    cv::Sobel(gray, gx, CV_32F, 1, 0, 3, 0.125);
    cv::Sobel(gray, gy, CV_32F, 0, 1, 3, 0.125);
    mask = TerrainMask(image);
    cv::erode(mask, mask, cv::getStructuringElement(cv::MORPH_ELLIPSE, {5, 5}));
    if (crossView && image.channels() >= 3) {
        cv::Mat bgr, hsv, land;
        if (image.channels() == 4) cv::cvtColor(image,bgr,cv::COLOR_BGRA2BGR);
        else bgr = image;
        cv::cvtColor(bgr,hsv,cv::COLOR_BGR2HSV);
        // Cross-view sparse matching is restricted to neutral cartographic
        // terrain. Sea contour lines, red map boundaries and bright UI are
        // not independent coast evidence. Dilate to retain both sides of edges.
        cv::inRange(hsv,cv::Scalar(0,0,45),cv::Scalar(180,110,200),land);
        cv::dilate(land,land,cv::getStructuringElement(cv::MORPH_ELLIPSE,{7,7}));
        cv::bitwise_and(mask,land,mask);
    }
}

// Dense signed-gradient agreement supplements sparse keypoints. This estimates
// relative motion only; callers must enforce a recent absolute anchor and must
// never renew that anchor using this result (no indefinitely accumulating drift).
inline bool TrackContours(const cv::Mat& reference, const cv::Mat& current, Motion& output, int maximumShift = 12,
    bool crossView = false) {
    output = {};
    if (reference.empty() || reference.size() != current.size() || current.size() != cv::Size(184, 184)) return false;
    if (maximumShift < 1 || maximumShift > 24) return false;
    cv::Mat ax, ay, am, bx, by, bm;
    Gradients(reference, ax, ay, am, crossView);
    Gradients(current, bx, by, bm, crossView);
    struct Vote { int x, y, support; double score; };
    std::vector<Vote> votes;
    for (int dy = -maximumShift; dy <= maximumShift; ++dy) for (int dx = -maximumShift; dx <= maximumShift; ++dx) {
        double dot = 0, aa = 0, bb = 0, xx = 0, yy = 0, xy = 0;
        int count = 0;
        for (int y = 4; y < 180; y += 2) for (int x = 4; x < 180; x += 2) {
            if (x+dx < 0 || y+dy < 0 || x+dx >= 184 || y+dy >= 184) continue;
            if (!am.at<uchar>(y,x) || !bm.at<uchar>(y+dy,x+dx)) continue;
            const double u = ax.at<float>(y,x), v = ay.at<float>(y,x);
            const double p = bx.at<float>(y+dy,x+dx), q = by.at<float>(y+dy,x+dx);
            if (u*u+v*v < 16 && p*p+q*q < 16) continue;
            dot += u*p+v*q; aa += u*u+v*v; bb += p*p+q*q;
            xx += u*u; yy += v*v; xy += u*v; ++count;
        }
        // Reject featureless images and a single straight edge, which cannot
        // constrain motion in two dimensions.
        if (count < 32 || aa < 1 || bb < 1 || (xx*yy-xy*xy) / (aa*aa) < 0.035) continue;
        votes.push_back({dx,dy,count,dot/std::sqrt(aa*bb)});
    }
    if (votes.empty()) return false;
    const auto best = *std::max_element(votes.begin(), votes.end(),
        [](const auto& a, const auto& b) { return a.score < b.score; });
    double runner = -1;
    for (const auto& vote : votes) if (std::hypot(vote.x-best.x, vote.y-best.y) >= 4)
        runner = std::max(runner, vote.score);
    output = {{static_cast<double>(best.x), static_cast<double>(best.y)}, best.score, best.score-runner, best.support};
    return std::abs(best.x) < maximumShift && std::abs(best.y) < maximumShift && best.score >= 0.85 && output.separation >= 0.08;
}

inline cv::Mat ViewportReference(const cv::Mat& nominalSnapshot, const cv::Point2f& arrow,
    double mapUnitsPerScreenPixel, double minimapScale = 194.0 / 184.0) {
    if (nominalSnapshot.empty() || !std::isfinite(mapUnitsPerScreenPixel) || mapUnitsPerScreenPixel <= 0) return {};
    if (!std::isfinite(minimapScale) || minimapScale <= 0) return {};
    const double sampleStep = minimapScale / mapUnitsPerScreenPixel;
    const cv::Mat inverse = (cv::Mat_<double>(2,3) << sampleStep,0,arrow.x-92*sampleStep,
        0,sampleStep,arrow.y-92*sampleStep);
    cv::Mat result;
    cv::warpAffine(nominalSnapshot,result,inverse,{184,184},cv::INTER_LINEAR | cv::WARP_INVERSE_MAP);
    return result;
}
}
