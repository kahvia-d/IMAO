#pragma once
#include "FeatureMatch.h"
#include "UniqueMapFeatures.h"
#include "ExactDescriptorMatcher.h"
#include <functional>
#include <span>

// Finish duplicate detection before allocating the output matrix: the owning hash
// and the materialized subset need not coexist at their peak sizes.
inline ImageFeatureData UniqueFeatureSubset(const ImageFeatureData& source,
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
