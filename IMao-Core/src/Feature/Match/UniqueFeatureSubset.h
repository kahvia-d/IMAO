#pragma once
#include "FeatureMatch.h"
#include "UniqueMapFeatures.h"
#include "ExactDescriptorMatcher.h"
#include <functional>
#include <span>

// Finish duplicate detection before allocating the output matrix: the owning hash
// and the materialized subset need not coexist at their peak sizes.
inline std::vector<uint32_t> UniqueFeatureRows(const ImageFeatureData& source,
    std::span<const uint32_t> rows, const std::function<bool()>& interrupted = {}) {
    std::vector<uint32_t> uniqueRows;
    uniqueRows.reserve(rows.size());
    {
        UniqueMapFeatures unique(rows.size());
        cv::Mat descriptor(1, source.imgDescriptors.cols, CV_32FC1);
        size_t checked = 0;
        for (auto row : rows) {
            if ((checked++ % 512) == 0 && interrupted && interrupted()) throw SearchInterrupted{};
            if (row >= source.imgKeypoints.size()) throw std::out_of_range("Map feature row outside keypoints");
            source.CopyDescriptorRowTo(static_cast<int>(row), descriptor.ptr<float>());
            if (unique.Insert(source.imgKeypoints[row], descriptor)) uniqueRows.push_back(row);
        }
    }
    if (interrupted && interrupted()) throw SearchInterrupted{};
    return uniqueRows;
}
inline ImageFeatureData UniqueFeatureSubset(const ImageFeatureData& source,
    std::span<const uint32_t> rows, const std::function<bool()>& interrupted = {}) {
    const auto uniqueRows = UniqueFeatureRows(source, rows, interrupted);
    ImageFeatureData result;
    if (uniqueRows.empty()) return result;
    result.imgKeypoints.reserve(uniqueRows.size());
    result.imgDescriptors.create(static_cast<int>(uniqueRows.size()), source.imgDescriptors.cols, CV_32FC1);
    for (size_t i = 0; i < uniqueRows.size(); ++i) {
        if ((i % 512) == 0 && interrupted && interrupted()) throw SearchInterrupted{};
        result.imgKeypoints.push_back(source.imgKeypoints[uniqueRows[i]]);
        source.CopyDescriptorRowTo(static_cast<int>(uniqueRows[i]), result.imgDescriptors.ptr<float>(static_cast<int>(i)));
    }
    return result;
}

inline ImageFeatureData UniqueEncodedFeatureSubset(const ImageFeatureData& source,
    std::span<const uint32_t> rows, const std::function<bool()>& interrupted = {}) {
    if (source.imgDescriptors.type() != CV_8UC1) return UniqueFeatureSubset(source, rows, interrupted);
    const auto uniqueRows = UniqueFeatureRows(source, rows, interrupted);
    ImageFeatureData result;
    if (uniqueRows.empty()) return result;
    result.imgKeypoints.reserve(uniqueRows.size());
    result.imgDescriptors.create(static_cast<int>(uniqueRows.size()), source.imgDescriptors.cols, CV_8UC1);
    for (size_t i = 0; i < uniqueRows.size(); ++i) {
        if ((i % 512) == 0 && interrupted && interrupted()) throw SearchInterrupted{};
        const int row = static_cast<int>(uniqueRows[i]);
        const auto range = std::upper_bound(source.descriptorEncoding.begin(), source.descriptorEncoding.end(), row,
            [](int value, const ImageFeatureData::DescriptorEncoding& item) { return value < item.endRow; });
        if (range == source.descriptorEncoding.end()) throw std::runtime_error("Invalid descriptor encoding range");
        result.imgKeypoints.push_back(source.imgKeypoints[row]);
        std::memcpy(result.imgDescriptors.ptr(static_cast<int>(i)), source.imgDescriptors.ptr(row), source.imgDescriptors.cols);
        if (!result.descriptorEncoding.empty() && result.descriptorEncoding.back().scale == range->scale &&
            result.descriptorEncoding.back().offset == range->offset)
            result.descriptorEncoding.back().endRow = static_cast<int>(i + 1);
        else result.descriptorEncoding.push_back({static_cast<int>(i + 1), range->scale, range->offset});
    }
    return result;
}
