#pragma once
#include <opencv2/opencv.hpp>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include "../../ImageProcessing/ImageProcessing.h"
#include <opencv2/xfeatures2d.hpp> 

struct ImageFeatureData {
    std::vector<cv::KeyPoint> imgKeypoints;
    cv::Mat imgDescriptors;

    struct DescriptorEncoding { int endRow; float scale; int offset; };
    std::vector<DescriptorEncoding> descriptorEncoding;

    void CopyDescriptorRowTo(int row, float* destination) const {
        if (row < 0 || row >= imgDescriptors.rows) throw std::out_of_range("Descriptor row outside storage");
        if (imgDescriptors.type() == CV_32FC1) {
            std::memcpy(destination, imgDescriptors.ptr<float>(row), imgDescriptors.cols * sizeof(float));
            return;
        }
        const auto range = std::upper_bound(descriptorEncoding.begin(), descriptorEncoding.end(), row,
            [](int value, const DescriptorEncoding& item) { return value < item.endRow; });
        if (imgDescriptors.type() != CV_8UC1 || range == descriptorEncoding.end())
            throw std::runtime_error("Invalid descriptor encoding range");
        const auto* codes = imgDescriptors.ptr<unsigned char>(row);
        for (int col = 0; col < imgDescriptors.cols; ++col)
            destination[col] = static_cast<float>(static_cast<int>(codes[col]) + range->offset) * range->scale;
    }

    cv::Mat FloatDescriptors() const {
        if (imgDescriptors.empty() || imgDescriptors.type() == CV_32FC1) return imgDescriptors;
        cv::Mat result(imgDescriptors.rows, imgDescriptors.cols, CV_32FC1);
        for (int row = 0; row < result.rows; ++row) CopyDescriptorRowTo(row, result.ptr<float>(row));
        return result;
    }

    // 默认构造函数
    ImageFeatureData() = default;

    // 原有构造函数
    ImageFeatureData(const std::vector<cv::KeyPoint>& keypoints, const cv::Mat& descriptors)
        : imgKeypoints(keypoints), imgDescriptors(descriptors) {
    }

    void Release() {
        std::vector<cv::KeyPoint>().swap(imgKeypoints);
        imgDescriptors.release();
        descriptorEncoding.clear();
    }
};

class FeatureMatch
{
	public:
		static std::vector<cv::DMatch> FindGoodMatchesBetweenMapAndMinMap(const ImageFeatureData& minMapFeatureData, const ImageFeatureData& mapFeatureData);
		static std::vector<cv::Point2f> GetTestImageCornersInOriginal(const ImageFeatureData& originalFeatureData, const ImageFeatureData& testFeatureData, const std::vector<cv::DMatch>& goodMatches, cv::Mat& testIame);
		static ImageFeatureData ExtractSurfFeatures(const cv::Mat& img);
		static ImageFeatureData ExtractSurfFeatures(cv::Ptr<cv::xfeatures2d::SURF>& surt, const cv::Mat& img);
		static std::vector<cv::DMatch> FindGoodMatches(const ImageFeatureData& originalFeatureData, const ImageFeatureData& testFeatureData, float ratioThresh, float maxDist, const cv::DescriptorMatcher::MatcherType& matcherType);
		static std::vector<cv::DMatch> FindGoodMatchesFLANN(const ImageFeatureData& originalFeatureData, const ImageFeatureData& testFeatureData, float ratioThresh, float maxDist);
};

