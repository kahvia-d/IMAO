#pragma once
#include "FeatureMatch.h"
#include "ExactDescriptorMatcher.h"
inline void MatchDescriptorsExactly(const ImageFeatureData& map, const cv::Mat& query,
    std::vector<std::vector<cv::DMatch>>& forward, std::vector<std::vector<cv::DMatch>>& reverse,
    const std::function<bool()>& interrupted = {}) {
    if (map.imgDescriptors.type() == CV_32FC1 || map.imgDescriptors.empty()) {
        MatchDescriptorsExactly(map.imgDescriptors, query, forward, reverse, interrupted);
        return;
    }
    // SURF descriptors have 128 columns: at most 4096 decoded rows (2 MiB),
    // alongside the shared matcher's at most 2 MiB distance batch.
    cv::Mat decoded;
    MatchDescriptorBatchesExactly(map.imgDescriptors.rows, query, [&](int begin, int end) {
        if (decoded.rows < end - begin) decoded.create(end - begin, map.imgDescriptors.cols, CV_32FC1);
        for (int row = begin; row < end; ++row) {
            if (((row - begin) % 512) == 0 && interrupted && interrupted()) throw SearchInterrupted{};
            map.CopyDescriptorRowTo(row, decoded.ptr<float>(row - begin));
        }
        return decoded.rowRange(0, end - begin);
    }, forward, reverse, interrupted);
}
