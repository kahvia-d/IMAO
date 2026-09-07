#pragma once

#include <opencv2/core.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// The terrain in a normalized gameplay minimap is north-up and its scale is
// fixed. A changing set of SURF detections must not introduce a fresh rotation
// or zoom into the player-center estimate on every frame.
inline cv::Mat FitMinimapTrackingTranslation(const std::vector<cv::Point2f>& source,
    const std::vector<cv::Point2f>& target, const cv::Size minimapSize,
    cv::Mat& consensusMask, double scale = 194.0 / 184.0) {
    constexpr double maximumError = 3.0;
    if (source.size() < 4 || source.size() != target.size() ||
        consensusMask.type() != CV_8UC1 || consensusMask.total() != source.size() ||
        !std::isfinite(scale) || scale <= 0.0 || minimapSize.empty()) return {};

    const cv::Point2d center(minimapSize.width / 2.0, minimapSize.height / 2.0);
    std::vector<cv::Point2d> votes;
    std::vector<double> xs, ys;
    votes.reserve(source.size());
    for (std::size_t i = 0; i < source.size(); ++i) {
        const cv::Point2d vote(target[i].x - scale * (source[i].x - center.x),
            target[i].y - scale * (source[i].y - center.y));
        if (!std::isfinite(vote.x) || !std::isfinite(vote.y)) return {};
        votes.push_back(vote);
        if (consensusMask.at<std::uint8_t>(static_cast<int>(i)) != 0) {
            xs.push_back(vote.x);
            ys.push_back(vote.y);
        }
    }
    if (xs.size() < 4) return {};
    const auto median = [](std::vector<double>& values) {
        std::sort(values.begin(), values.end());
        const auto middle = values.size() / 2;
        return values.size() % 2 == 0
            ? (values[middle - 1] + values[middle]) / 2.0 : values[middle];
    };
    cv::Point2d position(median(xs), median(ys));
    // Seed from the descriptor/RANSAC consensus, then refit only translation.
    // Recompute support against this final model; similarity-fit inliers must
    // not silently retain a pass after rotation and scale have been removed.
    for (int iteration = 0; iteration < 3; ++iteration) {
        cv::Point2d sum;
        int count = 0;
        for (const auto& vote : votes) {
            if (cv::norm(vote - position) > maximumError) continue;
            sum += vote;
            ++count;
        }
        if (count < 4) return {};
        position = sum * (1.0 / count);
    }
    consensusMask = cv::Mat::zeros(static_cast<int>(source.size()), 1, CV_8UC1);
    int count = 0;
    for (std::size_t i = 0; i < votes.size(); ++i) {
        if (cv::norm(votes[i] - position) > maximumError) continue;
        consensusMask.at<std::uint8_t>(static_cast<int>(i)) = 1;
        ++count;
    }
    if (count < 4) return {};
    return (cv::Mat_<double>(2, 3) << scale, 0.0, position.x - scale * center.x,
        0.0, scale, position.y - scale * center.y);
}
