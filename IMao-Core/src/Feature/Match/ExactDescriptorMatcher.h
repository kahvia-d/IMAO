#pragma once
#include <opencv2/features2d.hpp>
#include <algorithm>
#include <functional>
#include <limits>
#include <stdexcept>

struct SearchInterrupted : std::exception {
    const char* what() const noexcept override { return "visual search interrupted"; }
};

// Compute each exact L2 distance once, reusing it for both nearest-neighbour
// directions. Batches bound temporary storage and provide cancellation points.
inline void MatchDescriptorsExactly(const cv::Mat& map, const cv::Mat& query,
    std::vector<std::vector<cv::DMatch>>& forward,
    std::vector<std::vector<cv::DMatch>>& reverse,
    const std::function<bool()>& interrupted = {}) {
    forward.assign(map.rows, {});
    reverse.assign(query.rows, {});
    if (map.empty() || query.empty()) return;
    std::vector<float> reverseDistances(query.rows, std::numeric_limits<float>::max());
    std::vector<int> reverseRows(query.rows, -1);
    // Small minimaps have few descriptors: use larger row batches to avoid
    // repeatedly scheduling OpenCV workers, while capping the distance buffer.
    constexpr size_t distanceBufferBytes = 2 * 1024 * 1024;
    const int batchRows = static_cast<int>(std::max<size_t>(1, std::min<size_t>(4096,
        distanceBufferBytes / (static_cast<size_t>(query.rows) * sizeof(float)))));
    for (int begin = 0; begin < map.rows; begin += batchRows) {
        if (interrupted && interrupted()) throw SearchInterrupted{};
        cv::Mat distances;
        const int end = std::min(begin + batchRows, map.rows);
        cv::batchDistance(map.rowRange(begin, end), query, distances, CV_32F, cv::noArray(), cv::NORM_L2);
        for (int row = begin; row < end; ++row) {
            const float* values = distances.ptr<float>(row - begin);
            int first = -1, second = -1;
            for (int column = 0; column < query.rows; ++column) {
                if (first < 0 || values[column] < values[first]) { second = first; first = column; }
                else if (second < 0 || values[column] < values[second]) second = column;
                if (values[column] < reverseDistances[column]) {
                    reverseDistances[column] = values[column]; reverseRows[column] = row;
                }
            }
            if (first >= 0) forward[row].emplace_back(row, first, values[first]);
            if (second >= 0) forward[row].emplace_back(row, second, values[second]);
        }
    }
    for (int row = 0; row < query.rows; ++row)
        if (reverseRows[row] >= 0) reverse[row].emplace_back(row, reverseRows[row], reverseDistances[row]);
}
