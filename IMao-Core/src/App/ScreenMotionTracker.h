#pragma once

#include "../Runtime/OverlayMotion.h"
#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>
#include <array>
#include <vector>

// Image registration only. The returned pose always describes observed pixels;
// elapsed time, render cadence and localization coordinates never enter the fit.
class ScreenMotionTracker {
public:
    void Reset();
    bool Anchor(const cv::Mat& image, bool minimap);
    bool Track(const cv::Mat& image, OverlayScreenTransform& imageTransform);

private:
    static constexpr int kLevels = 3;
    static constexpr int kPatchRadius = 4;
    static constexpr int kPatchPixels = 81;
    struct Patch {
        cv::Point2f point;
        std::array<float, kPatchPixels> value{}, gx{}, gy{};
        double xx = 0, xy = 0, yy = 0;
        bool valid = false;
    };
    struct Feature { std::array<Patch, kLevels> patch; };

    bool Prepare(const cv::Mat& image, cv::Mat& gray, cv::Mat& mask) const;
    bool SetReference(const cv::Mat& gray, const cv::Mat& mask);
    static Patch MakePatch(const cv::Mat& image, const cv::Mat& gx, const cv::Mat& gy, const cv::Point2f& point);
    bool Align(const Feature& feature, const std::array<cv::Mat, kLevels>& current,
        double scale, const cv::Point2f& offset, cv::Point2f& target, bool coarse) const;
    bool Fit(const std::vector<cv::Point2f>& source, const std::vector<cv::Point2f>& target,
        OverlayScreenTransform& result, double maximumResidual = 1.0) const;
    bool DescriptorFit(const cv::Mat& gray, const cv::Mat& mask, OverlayScreenTransform& result);

    bool minimap_ = false;
    cv::Size inputSize_, trackingSize_;
    double imageRatio_ = 1;
    cv::Mat reference_, referenceMask_, descriptors_;
    std::array<cv::Mat, kLevels> referencePyramid_;
    std::vector<cv::KeyPoint> keys_;
    std::vector<Feature> features_;
    OverlayScreenTransform anchorToReference_, referenceToCurrent_;
};
