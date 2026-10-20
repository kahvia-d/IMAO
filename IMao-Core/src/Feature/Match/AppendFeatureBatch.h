#pragma once
#include "FeatureMatch.h"
#include <limits>
#include <string>

// Repeated cv::vconcat calls reallocate and copy the large base map descriptor
// matrix for every optional pack.  On lower-memory machines that causes paging
// and makes a few small extension packs appear to hang the application.
inline bool AppendFeatureBatch(ImageFeatureData& destination,
    const std::vector<const ImageFeatureData*>& additions, std::string& error) {
    if (additions.empty()) return true;
    // A snapshot that selects no base atlas arrives here empty: the first usable pack then
    // seeds the map instead of being rejected as an incomplete base. The seed also defines
    // the descriptor shape every later pack has to match. It is a copy, so that same pack
    // must be skipped below - counting or appending it again would duplicate its rows and
    // shift every later pack's row base away from what its visual shard recorded.
    const ImageFeatureData* seeded = nullptr;
    if (destination.imgKeypoints.empty() || destination.imgDescriptors.empty()) {
        for (const auto* addition : additions) {
            if (addition == nullptr || addition->imgKeypoints.empty() || addition->imgDescriptors.empty()) continue;
            if (addition->imgDescriptors.rows != static_cast<int>(addition->imgKeypoints.size())) {
                error = "optional map feature descriptors are incompatible with their keypoints";
                return false;
            }
            destination = *addition;
            seeded = addition;
            break;
        }
    }
    if (destination.imgKeypoints.empty() || destination.imgDescriptors.empty() ||
        destination.imgDescriptors.rows != static_cast<int>(destination.imgKeypoints.size())) {
        error = "base map feature data is incomplete";
        return false;
    }

    bool packed = destination.imgDescriptors.type() == CV_8UC1;
    std::size_t totalKeypoints = destination.imgKeypoints.size();
    for (const auto* addition : additions) {
        if (addition == nullptr || addition == seeded || addition->imgKeypoints.empty() || addition->imgDescriptors.empty()) continue;
        if (addition->imgDescriptors.rows != static_cast<int>(addition->imgKeypoints.size()) ||
            (addition->imgDescriptors.type() != CV_32FC1 && addition->imgDescriptors.type() != CV_8UC1) ||
            addition->imgDescriptors.cols != destination.imgDescriptors.cols) {
            error = "optional map feature descriptors are incompatible with the base map";
            return false;
        }
        if (addition->imgKeypoints.size() >
            std::numeric_limits<std::size_t>::max() - totalKeypoints) {
            error = "optional map feature count overflow";
            return false;
        }
        packed = packed && addition->imgDescriptors.type() == CV_8UC1;
        totalKeypoints += addition->imgKeypoints.size();
    }
    if (totalKeypoints > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        error = "optional map feature row count exceeds OpenCV limits";
        return false;
    }

    ImageFeatureData merged;
    merged.imgKeypoints.reserve(totalKeypoints);
    merged.imgDescriptors.create(static_cast<int>(totalKeypoints), destination.imgDescriptors.cols,
        packed ? CV_8UC1 : CV_32FC1);
    int row = 0;
    const auto append = [&merged, &row, packed](const ImageFeatureData& source) {
        if (source.imgKeypoints.empty() || source.imgDescriptors.empty()) return;
        merged.imgKeypoints.insert(merged.imgKeypoints.end(), source.imgKeypoints.begin(), source.imgKeypoints.end());
        const auto nextRow = row + source.imgDescriptors.rows;
        if (packed) {
            source.imgDescriptors.copyTo(merged.imgDescriptors.rowRange(row, nextRow));
            for (const auto& encoding : source.descriptorEncoding)
                merged.descriptorEncoding.push_back({row + encoding.endRow, encoding.scale, encoding.offset});
        } else {
            for (int i = 0; i < source.imgDescriptors.rows; ++i)
                source.CopyDescriptorRowTo(i, merged.imgDescriptors.ptr<float>(row + i));
        }
        row = nextRow;
    };
    append(destination);
    for (const auto* addition : additions) {
        if (addition != nullptr && addition != seeded &&
            !addition->imgKeypoints.empty() && !addition->imgDescriptors.empty()) {
            append(*addition);
        }
    }
    destination = std::move(merged);
    return true;
}
