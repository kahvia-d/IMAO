#include "ScreenMotionTracker.h"
#include "MapViewportGeometry.h"

#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>

namespace {
constexpr int kMinimumSupport = 12;

float Sample(const cv::Mat& image, float x, float y) {
    const int ix = static_cast<int>(x), iy = static_cast<int>(y);
    const float dx = x - ix, dy = y - iy;
    const auto* row = image.ptr<float>(iy);
    const auto* next = image.ptr<float>(iy + 1);
    return (row[ix] * (1 - dx) + row[ix + 1] * dx) * (1 - dy) +
        (next[ix] * (1 - dx) + next[ix + 1] * dx) * dy;
}

double Median(std::vector<double> values) {
    const auto middle = values.begin() + values.size() / 2;
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

void Pyramid(const cv::Mat& gray, std::array<cv::Mat, 3>& result, bool antialias) {
    gray.convertTo(result[0], CV_32F);
    // Subpixel registration resamples both directions. Comparing unfiltered
    // single-pixel map strokes at different pixel phases confuses interpolation
    // artifacts with changed terrain. Put both images in the same bandwidth;
    // retain the strict photometric and geometric gates on that shared image.
    if (antialias) cv::GaussianBlur(result[0], result[0], {3, 3}, 0.75);
    cv::pyrDown(result[0], result[1]);
    cv::pyrDown(result[1], result[2]);
}

OverlayScreenTransform Compose(const OverlayScreenTransform& first, const OverlayScreenTransform& second) {
    return {first.scale * second.scale,
        {first.offset.x * second.scale + second.offset.x, first.offset.y * second.scale + second.offset.y}};
}
}

void ScreenMotionTracker::Reset() {
    reference_.release(); referenceMask_.release(); descriptors_.release();
    for (auto& level : referencePyramid_) level.release();
    keys_.clear(); features_.clear();
    inputSize_ = {}; trackingSize_ = {}; imageRatio_ = 1;
    anchorToReference_ = {}; referenceToCurrent_ = {};
}

bool ScreenMotionTracker::Prepare(const cv::Mat& image, cv::Mat& gray, cv::Mat& mask) const {
    if (image.empty() || image.depth() != CV_8U || image.size() != inputSize_ ||
        (image.channels() != 1 && image.channels() != 3 && image.channels() != 4)) return false;
    cv::Mat resized;
    if (image.size() != trackingSize_) cv::resize(image, resized, trackingSize_, 0, 0, cv::INTER_AREA);
    else resized = image;
    if (resized.channels() == 1) gray = resized;
    else cv::cvtColor(resized, gray, resized.channels() == 4 ? cv::COLOR_BGRA2GRAY : cv::COLOR_BGR2GRAY);

    mask = cv::Mat::zeros(gray.size(), CV_8UC1);
    if (minimap_) {
        const cv::Point center(gray.cols / 2, gray.rows / 2);
        const int radius = std::min(gray.cols, gray.rows);
        cv::circle(mask, center, cvRound(radius * 0.44), cv::Scalar(255), cv::FILLED);
        // Exclude the player and nearby heading graphics. The translucent cone
        // outside this disk is rejected by the distributed terrain consensus.
        cv::circle(mask, center, cvRound(radius * 0.22), cv::Scalar(0), cv::FILLED);
        cv::Mat dynamic;
        cv::threshold(gray, dynamic, 205, 255, cv::THRESH_BINARY);
        if (resized.channels() > 1) {
            for (int y = 0; y < resized.rows; ++y) {
                const auto* row = resized.ptr<unsigned char>(y);
                auto* excluded = dynamic.ptr<unsigned char>(y);
                for (int x = 0; x < resized.cols; ++x) {
                    const auto* pixel = row + x * resized.channels();
                    const int high = std::max({pixel[0], pixel[1], pixel[2]});
                    const int low = std::min({pixel[0], pixel[1], pixel[2]});
                    if (high > 105 && high - low > 65) excluded[x] = 255;
                }
            }
        }
        cv::dilate(dynamic, dynamic, cv::getStructuringElement(cv::MORPH_ELLIPSE, {9, 9}));
        mask.setTo(0, dynamic);
    } else {
        const int margin = 12;
        if (gray.cols <= margin * 2 || gray.rows <= margin * 2) return false;
        mask(cv::Rect(margin, margin, gray.cols - margin * 2, gray.rows - margin * 2)).setTo(255);
    }
    return cv::countNonZero(mask) >= 400;
}

bool ScreenMotionTracker::Anchor(const cv::Mat& image, bool minimap) {
    Reset();
    if (image.empty() || image.cols < 64 || image.rows < 64) return false;
    minimap_ = minimap;
    inputSize_ = image.size();
    imageRatio_ = std::min(1.0, 320.0 / std::max(image.cols, image.rows));
    trackingSize_ = {cvRound(image.cols * imageRatio_), cvRound(image.rows * imageRatio_)};
    cv::Mat gray, mask;
    if (!Prepare(image, gray, mask) || !SetReference(gray, mask)) { Reset(); return false; }
    return true;
}

ScreenMotionTracker::Patch ScreenMotionTracker::MakePatch(const cv::Mat& image, const cv::Mat& gx,
    const cv::Mat& gy, const cv::Point2f& point) {
    Patch patch;
    patch.point = point;
    if (point.x < kPatchRadius + 1 || point.y < kPatchRadius + 1 ||
        point.x >= image.cols - kPatchRadius - 2 || point.y >= image.rows - kPatchRadius - 2) return patch;
    int index = 0;
    double brightness = 0;
    for (int y = -kPatchRadius; y <= kPatchRadius; ++y) for (int x = -kPatchRadius; x <= kPatchRadius; ++x, ++index) {
        const float px = point.x + x, py = point.y + y;
        patch.value[index] = Sample(image, px, py);
        patch.gx[index] = Sample(gx, px, py);
        patch.gy[index] = Sample(gy, px, py);
        brightness += patch.value[index];
        patch.xx += patch.gx[index] * patch.gx[index];
        patch.xy += patch.gx[index] * patch.gy[index];
        patch.yy += patch.gy[index] * patch.gy[index];
    }
    for (auto& value : patch.value) value -= static_cast<float>(brightness / kPatchPixels);
    patch.valid = patch.xx * patch.yy - patch.xy * patch.xy > 1.0;
    return patch;
}

bool ScreenMotionTracker::SetReference(const cv::Mat& gray, const cv::Mat& mask) {
    std::vector<cv::Point2f> corners;
    cv::goodFeaturesToTrack(gray, corners, 96, 0.01, std::max(5.0, gray.cols / 40.0), mask);
    if (corners.size() < kMinimumSupport) return false;
    std::array<cv::Mat, kLevels> pyramid, gx, gy;
    Pyramid(gray, pyramid, !minimap_);
    for (int level = 0; level < kLevels; ++level) {
        cv::Sobel(pyramid[level], gx[level], CV_32F, 1, 0, 3, 0.125);
        cv::Sobel(pyramid[level], gy[level], CV_32F, 0, 1, 3, 0.125);
    }
    std::vector<Feature> features;
    for (const auto& corner : corners) {
        Feature feature;
        for (int level = 0; level < kLevels; ++level) {
            feature.patch[level] = MakePatch(pyramid[level], gx[level], gy[level], corner * (1.f / (1 << level)));
        }
        if (feature.patch[0].valid) features.push_back(feature);
    }
    if (features.size() < kMinimumSupport) return false;
    features_ = std::move(features);
    referencePyramid_ = std::move(pyramid);
    reference_ = gray.clone(); referenceMask_ = mask.clone();
    keys_.clear(); descriptors_.release();
    return true;
}

bool ScreenMotionTracker::Align(const Feature& feature, const std::array<cv::Mat, kLevels>& current,
    double scale, const cv::Point2f& offset, cv::Point2f& target, bool coarse) const {
    target = feature.patch[0].point * static_cast<float>(scale) + offset;
    for (int level = coarse ? kLevels - 1 : 0; level >= 0; --level) {
        const auto& patch = feature.patch[level];
        if (!patch.valid) continue;
        const auto& image = current[level];
        cv::Point2f q = target * (1.f / (1 << level));
        const double determinant = patch.xx * patch.yy - patch.xy * patch.xy;
        double lastError = 0, lastCorrelation = 0, lastStep = 0;
        for (int iteration = 0; iteration < 10; ++iteration) {
            const double radius = kPatchRadius * scale + 1;
            if (q.x < radius || q.y < radius || q.x >= image.cols - radius - 1 || q.y >= image.rows - radius - 1) return false;
            std::array<float, kPatchPixels> values;
            double brightness = 0;
            int index = 0;
            for (int y = -kPatchRadius; y <= kPatchRadius; ++y) for (int x = -kPatchRadius; x <= kPatchRadius; ++x, ++index) {
                values[index] = Sample(image, q.x + static_cast<float>(x * scale), q.y + static_cast<float>(y * scale));
                brightness += values[index];
            }
            brightness /= kPatchPixels;
            double ex = 0, ey = 0, error = 0, cross = 0, referenceEnergy = 0, targetEnergy = 0;
            for (int i = 0; i < kPatchPixels; ++i) {
                const double centered = values[i] - brightness;
                const double difference = centered - patch.value[i];
                ex += patch.gx[i] * difference; ey += patch.gy[i] * difference;
                error += difference * difference;
                if (!minimap_ && level == 0) {
                    cross += centered * patch.value[i];
                    referenceEnergy += patch.value[i] * patch.value[i];
                    targetEnergy += centered * centered;
                }
            }
            const cv::Point2f step(static_cast<float>(scale * (patch.yy * ex - patch.xy * ey) / determinant),
                static_cast<float>(scale * (patch.xx * ey - patch.xy * ex) / determinant));
            if (!std::isfinite(step.x) || !std::isfinite(step.y) || cv::norm(step) > 4.0) return false;
            q -= step;
            lastError = error / kPatchPixels;
            lastStep = cv::norm(step);
            if (!minimap_ && level == 0) lastCorrelation = referenceEnergy > 1.0 && targetEnergy > 1.0
                ? cross / std::sqrt(referenceEnergy * targetEnergy) : 0.0;
            if (lastStep < 0.015) break;
        }
        // Prevent a decorrelated patch on an icon or newly exposed terrain from
        // voting for a geometric model merely because the solver stopped.
        if (level == 0 && lastError > 90.0) return false;
        // A low-contrast map can have a small absolute gray error at a wholly
        // wrong location after a fast drag. Require actual texture correlation
        // and a completed solve before it can suppress descriptor reacquisition.
        if (!minimap_ && level == 0 && (lastCorrelation < 0.90 || lastStep > 0.15)) return false;
        target = q * static_cast<float>(1 << level);
    }
    return true;
}

bool ScreenMotionTracker::Fit(const std::vector<cv::Point2f>& source,
    const std::vector<cv::Point2f>& target, OverlayScreenTransform& result, double maximumResidual) const {
    if (source.size() < kMinimumSupport) return false;
    if (minimap_) {
        std::vector<double> dx, dy;
        for (size_t i = 0; i < source.size(); ++i) { dx.push_back(target[i].x - source[i].x); dy.push_back(target[i].y - source[i].y); }
        result = {1.0, {Median(dx), Median(dy)}};
    } else {
        cv::Mat inliers;
        const auto fit = FitMapViewportTransform(source, target, inliers);
        if (fit.empty()) return false;
        result = {fit.at<double>(0, 0), {fit.at<double>(0, 2), fit.at<double>(1, 2)}};
    }
    if (result.scale < 0.75 || result.scale > 1.34) return false;
    std::vector<cv::Point2f> support, destinations, hull;
    for (size_t i = 0; i < source.size(); ++i) {
        const auto projected = result.Apply({source[i].x, source[i].y});
        if (std::hypot(projected.x - target[i].x, projected.y - target[i].y) <= maximumResidual) {
            support.push_back(source[i]); destinations.push_back(target[i]);
        }
    }
    if (support.size() < kMinimumSupport || support.size() < source.size() * 0.65) return false;
    const auto bounds = cv::boundingRect(support);
    cv::convexHull(support, hull);
    if (bounds.width < trackingSize_.width * 0.30 || bounds.height < trackingSize_.height * 0.30 ||
        cv::contourArea(hull) < trackingSize_.area() * 0.045) return false;
    // Refit strictly from the one-pixel consensus. Descriptor fallback therefore
    // has the same final precision and spatial evidence as the direct path.
    cv::Point2d from{}, to{};
    for (size_t i = 0; i < support.size(); ++i) { from += cv::Point2d(support[i]); to += cv::Point2d(destinations[i]); }
    from *= 1.0 / support.size(); to *= 1.0 / support.size();
    if (!minimap_) {
        double numerator = 0, denominator = 0;
        for (size_t i = 0; i < support.size(); ++i) {
            const auto a = cv::Point2d(support[i]) - from, b = cv::Point2d(destinations[i]) - to;
            numerator += a.dot(b); denominator += a.dot(a);
        }
        if (denominator < 1.0) return false;
        result.scale = numerator / denominator;
    }
    result.offset = {to.x - from.x * result.scale, to.y - from.y * result.scale};
    return true;
}

bool ScreenMotionTracker::DescriptorFit(const cv::Mat& gray, const cv::Mat& mask, OverlayScreenTransform& result) {
    auto orb = cv::ORB::create(420, 1.2f, 5, 12, 0, 2, cv::ORB::HARRIS_SCORE, 21, 10);
    if (descriptors_.empty()) orb->detectAndCompute(reference_, referenceMask_, keys_, descriptors_);
    std::vector<cv::KeyPoint> currentKeys;
    cv::Mat currentDescriptors;
    orb->detectAndCompute(gray, mask, currentKeys, currentDescriptors);
    if (descriptors_.empty() || currentDescriptors.empty()) return false;
    std::vector<std::vector<cv::DMatch>> matches;
    cv::BFMatcher(cv::NORM_HAMMING).knnMatch(descriptors_, currentDescriptors, matches, 2);
    std::vector<cv::Point2f> from, to;
    std::vector<bool> used(currentKeys.size());
    for (const auto& pair : matches) {
        if (pair.size() < 2 || pair[0].distance >= 0.72f * pair[1].distance || used[pair[0].trainIdx]) continue;
        used[pair[0].trainIdx] = true;
        from.push_back(keys_[pair[0].queryIdx].pt); to.push_back(currentKeys[pair[0].trainIdx].pt);
    }
    // ORB keypoints are quantized and may come from different pyramid levels.
    // This three-pixel model is only a search seed, never a drawable transform;
    // Track still requires the same precise direct-image evidence afterwards.
    return Fit(from, to, result, 3.0);
}

bool ScreenMotionTracker::Track(const cv::Mat& image, OverlayScreenTransform& imageTransform) {
    imageTransform = {};
    if (reference_.empty()) return false;
    cv::Mat gray, mask;
    if (!Prepare(image, gray, mask)) return false;
    std::array<cv::Mat, kLevels> pyramid;
    Pyramid(gray, pyramid, !minimap_);
    cv::Mat currentGx, currentGy;
    if (!minimap_) {
        cv::Sobel(pyramid[0], currentGx, CV_32F, 1, 0, 3, 0.125);
        cv::Sobel(pyramid[0], currentGy, CV_32F, 0, 1, 3, 0.125);
    }
    OverlayScreenTransform measured;
    const auto align = [&](const OverlayScreenTransform& seed, bool coarse) {
        std::vector<cv::Point2f> from, to;
        const cv::Point2f offset(static_cast<float>(seed.offset.x), static_cast<float>(seed.offset.y));
        for (const auto& feature : features_) {
            cv::Point2f target;
            if (!Align(feature, pyramid, seed.scale, offset, target, coarse)) continue;
            const cv::Point pixel(cvRound(target.x), cvRound(target.y));
            if (pixel.x < 0 || pixel.y < 0 || pixel.x >= mask.cols || pixel.y >= mask.rows || mask.at<unsigned char>(pixel) == 0) continue;
            if (!minimap_) {
                Feature reverse;
                reverse.patch[0] = MakePatch(pyramid[0], currentGx, currentGy, target);
                if (!reverse.patch[0].valid) continue;
                const cv::Point2f backwardOffset = feature.patch[0].point - target * static_cast<float>(1.0 / seed.scale);
                cv::Point2f backward;
                if (!Align(reverse, referencePyramid_, 1.0 / seed.scale, backwardOffset, backward, false) ||
                    cv::norm(backward - feature.patch[0].point) > 0.75) continue;
            }
            from.push_back(feature.patch[0].point); to.push_back(target);
        }
        return Fit(from, to, measured);
    };
    // Ordinary capture-to-capture motion is small. Start at full resolution so
    // coarse patches cannot mix moving heading graphics into nearby terrain;
    // the image pyramid is a measured large-displacement fallback only.
    if (!align(referenceToCurrent_, false) && !align(referenceToCurrent_, true)) {
        OverlayScreenTransform coarseSeed;
        if (!DescriptorFit(gray, mask, coarseSeed) || !align(coarseSeed, false)) return false;
    }
    referenceToCurrent_ = measured;
    imageTransform = Compose(anchorToReference_, measured);
    imageTransform.offset.x /= imageRatio_; imageTransform.offset.y /= imageRatio_;
    const auto center = measured.Apply({trackingSize_.width / 2.0, trackingSize_.height / 2.0});
    const double shift = std::hypot(center.x - trackingSize_.width / 2.0, center.y - trackingSize_.height / 2.0);
    // Retain an image reference across slow movement instead of integrating the
    // fractional resampling error at every capture. Rebase before overlap ends.
    if ((shift > 18.0 || std::abs(measured.scale - 1.0) > 0.06) && SetReference(gray, mask)) {
        anchorToReference_ = Compose(anchorToReference_, measured);
        referenceToCurrent_ = {};
    }
    return true;
}
