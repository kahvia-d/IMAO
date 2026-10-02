#pragma once

// Everything a full-screen search does to decide *which map features it compares against*, kept in one
// place so the shipped path and the offline comparison cannot drift apart.
//
// The shipped path copies the selected rows into a de-duplicated candidate set and trains a FLANN index
// over that copy, once per window, caching a few of them. The alternative under evaluation compares
// against the map rows directly - no copy, no index, no cache - and de-duplicates the matched pairs
// instead of the candidates. Both are defined here, and both are driven by the same row selection, so a
// measurement says what the difference in matching actually costs rather than what two separate
// implementations happen to do (2026-10-02).

#include "../Feature/Match/ExactDescriptorMatcher.h"
#include "../Feature/Match/FeatureMatch.h"
#include "../Feature/Match/UniqueMapFeatures.h"
#include "../Feature/RuntimeFeatureRepository.h"
#include "../Feature/VisualIndex/MapVisualIndex.h"
#include "WorldSearchPrior.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace MapViewportCandidates {

inline int TileScene(const RuntimeFeatureResources& resources, std::size_t index) {
    return index < resources.baseVisualTileCount ? Scene::SceneNameToId("World") :
        resources.visualIndex.tiles[index].sceneId;
}

inline bool IntersectsPrior(const MapVisualTile& tile, const WorldSearchPrior& prior) {
    const double nearestX = std::clamp(prior.centerMapCoordinate.x,
        static_cast<double>(tile.minX), static_cast<double>(tile.maxX));
    const double nearestY = std::clamp(prior.centerMapCoordinate.y,
        static_cast<double>(tile.minY), static_cast<double>(tile.maxY));
    return std::hypot(prior.centerMapCoordinate.x - nearestX,
        prior.centerMapCoordinate.y - nearestY) <= prior.radius;
}

inline std::vector<std::uint32_t> SelectSceneTileIndices(const RuntimeFeatureResources& resources,
    int sceneId, const std::optional<WorldSearchPrior>& prior) {
    // Build the verification set from the visual-index rows rather than
    // merely filtering all map keypoints by X/Y.  This keeps an independent
    // state with overlapping internal coordinates out of a World search, and
    // includes every approved World supplement (such as BlackShores) exactly
    // once.
    if (!resources.visualIndexReady || resources.visualIndex.tiles.empty()) {
        return {};
    }

    std::vector<std::uint32_t> selected;
    for (std::size_t tileIndex = 0; tileIndex < resources.visualIndex.tiles.size(); ++tileIndex) {
        const auto& tile = resources.visualIndex.tiles[tileIndex];
        if (TileScene(resources, tileIndex) != sceneId ||
            (prior.has_value() && prior->valid && !IntersectsPrior(tile, *prior))) {
            continue;
        }
        selected.push_back(static_cast<std::uint32_t>(tileIndex));
    }
    return selected;
}

// The rows the chosen tiles contribute, in map-row order, with rows the runtime has disabled or that
// fall outside the descriptor storage left out. Ascending order is part of the contract: the cached-key
// path relies on the same tile set producing the same sequence.
inline std::vector<std::uint32_t> SelectCandidateRows(const RuntimeFeatureResources& resources,
    const std::vector<std::uint32_t>& tileIndices) {
    if (tileIndices.empty()) return {};
    std::vector<unsigned char> selected(resources.map.imgKeypoints.size(), 0);
    for (const auto tileIndex : tileIndices) {
        if (tileIndex >= resources.visualIndex.tiles.size()) continue;
        const auto& tile = resources.visualIndex.tiles[tileIndex];
        const std::size_t first = tile.featureRowOffset;
        const std::size_t last = first + tile.featureRowCount;
        if (last > resources.visualIndex.featureRows.size()) continue;
        for (std::size_t index = first; index < last; ++index) {
            const auto row = resources.visualIndex.featureRows[index];
            if (row < selected.size()) selected[row] = 1;
        }
    }
    std::vector<std::uint32_t> rows;
    for (std::size_t row = 0; row < selected.size(); ++row) {
        if (selected[row] && resources.FeatureRowEnabled(row)) rows.push_back(static_cast<std::uint32_t>(row));
    }
    return rows;
}

// The shipped path: one owned, de-duplicated copy of those rows. Two rows describing the same physical
// map point collapse here, which is what the geometry fit wants - one vote per point - and it is also
// what makes the copy necessary and what the copy costs.
inline ImageFeatureData SelectSceneCandidates(const RuntimeFeatureResources& resources,
    const std::vector<std::uint32_t>& tileIndices) {
    ImageFeatureData candidates;
    const auto rows = SelectCandidateRows(resources, tileIndices);
    if (rows.empty()) return candidates;
    UniqueMapFeatures unique;
    candidates.imgKeypoints.reserve(rows.size());
    for (const auto row : rows) {
        if (!unique.Insert(resources.map.imgKeypoints[row],
                resources.map.imgDescriptors.row(static_cast<int>(row)))) continue;
        candidates.imgKeypoints.push_back(resources.map.imgKeypoints[row]);
        candidates.imgDescriptors.push_back(resources.map.imgDescriptors.row(static_cast<int>(row)));
    }
    return candidates;
}

inline std::string TileSetKey(const std::vector<std::uint32_t>& tileIndices) {
    std::string key;
    key.reserve(tileIndices.size() * 6);
    for (const auto tileIndex : tileIndices) {
        key += std::to_string(tileIndex);
        key.push_back(',');
    }
    return key;
}

inline std::vector<cv::DMatch> FilterGoodMatches(const std::vector<std::vector<cv::DMatch>>& pairs) {
    std::vector<cv::DMatch> goodMatches;
    for (const auto& pair : pairs) {
        if (pair.size() < 2 || pair[0].distance >= 0.62f * pair[1].distance || pair[0].distance > 0.50f) continue;
        goodMatches.push_back(pair[0]);
    }
    return goodMatches;
}

// Exhaustive nearest and second-nearest map row for every query descriptor, over the given rows only. The
// same batching and cancellation points as MatchDescriptorsExactly, but keyed by the row list rather than by
// the whole matrix, and taking both places per query - which is what the ratio test needs.
inline void MatchDescriptorNeighbours(const cv::Mat& queryDescriptors, const cv::Mat& mapDescriptors,
    const std::vector<std::uint32_t>& rows, std::vector<float>& best, std::vector<float>& second,
    std::vector<int>& bestRow, const std::function<bool()>& interrupted = {}) {
    if (rows.empty() || queryDescriptors.empty() || mapDescriptors.empty()) return;
    std::vector<int> rowPositions(mapDescriptors.rows, -1);
    for (std::size_t position = 0; position < rows.size(); ++position) {
        const auto row = rows[position];
        if (row < rowPositions.size()) rowPositions[row] = static_cast<int>(position);
    }
    // Bound the temporary distance block the way MatchDescriptorsExactly does, so a large query cannot make
    // the batch allocate without limit.
    constexpr std::size_t distanceBufferBytes = 2 * 1024 * 1024;
    const int batchRows = static_cast<int>(std::max<std::size_t>(1, std::min<std::size_t>(4096,
        distanceBufferBytes / (static_cast<std::size_t>(queryDescriptors.rows) * sizeof(float)))));
    for (int begin = 0; begin < mapDescriptors.rows; begin += batchRows) {
        if (interrupted && interrupted()) throw SearchInterrupted{};
        const int end = std::min(begin + batchRows, mapDescriptors.rows);
        cv::Mat distances;
        cv::batchDistance(mapDescriptors.rowRange(begin, end), queryDescriptors, distances, CV_32F,
            cv::noArray(), cv::NORM_L2);
        for (int row = begin; row < end; ++row) {
            const int position = rowPositions[row];
            if (position < 0) continue;
            const float* values = distances.ptr<float>(row - begin);
            for (int column = 0; column < queryDescriptors.rows; ++column) {
                const float distance = values[column];
                if (distance < best[column]) {
                    second[column] = best[column];
                    best[column] = distance;
                    bestRow[column] = position;
                } else if (distance < second[column]) {
                    second[column] = distance;
                }
            }
        }
    }
}

// The alternative: match every chosen row against the query, then keep the pairs that pass the same ratio
// test the shipped path applies - and in the same direction. The ratio has to be taken per *query*
// descriptor, over its two nearest map rows; taking it per map row instead, over the row's two nearest
// query descriptors, measures a different thing and rejects essentially everything, because map rows are
// far more numerous than query descriptors and many of them sit close together (2026-10-02).
inline std::vector<cv::DMatch> MatchRowsExactly(const RuntimeFeatureResources& resources,
    const std::vector<std::uint32_t>& rows, const cv::Mat& queryDescriptors,
    const std::function<bool()>& interrupted = {}) {
    std::vector<cv::DMatch> good;
    if (rows.empty() || queryDescriptors.empty()) return good;

    // Two nearest map rows per query descriptor, computed from the exhaustive pass rather than asked for in
    // reverse, so the ratio below is the same quantity the shipped matcher's knnMatch(k=2) produces.
    std::vector<float> best(queryDescriptors.rows, std::numeric_limits<float>::max());
    std::vector<float> second(queryDescriptors.rows, std::numeric_limits<float>::max());
    std::vector<int> bestRow(queryDescriptors.rows, -1);
    MatchDescriptorNeighbours(queryDescriptors, resources.map.imgDescriptors, rows, best, second, bestRow,
        interrupted);

    good.reserve(queryDescriptors.rows);
    for (int index = 0; index < queryDescriptors.rows; ++index) {
        if (bestRow[index] < 0) continue;
        if (second[index] > 0.0f && best[index] >= 0.62f * second[index]) continue;
        if (best[index] > 0.50f) continue;
        good.emplace_back(static_cast<int>(bestRow[index]), index, best[index]);
    }
    return good;
}

// Rows that describe the same physical point, removed from a pair list so each point votes once. The
// windowed path gets this for free from its de-duplicated copy; pair-level de-duplication is what makes
// the copy unnecessary.
inline std::vector<cv::DMatch> DeduplicatePairs(const RuntimeFeatureResources& resources,
    const std::vector<std::uint32_t>& rows, const std::vector<cv::DMatch>& matches) {
    std::vector<cv::DMatch> kept;
    kept.reserve(matches.size());
    UniqueMapFeatures unique(matches.size());
    for (const auto& match : matches) {
        if (match.trainIdx < 0 || match.trainIdx >= static_cast<int>(rows.size())) continue;
        const auto row = rows[static_cast<std::size_t>(match.trainIdx)];
        if (!unique.Insert(resources.map.imgKeypoints[row],
                resources.map.imgDescriptors.row(static_cast<int>(row)))) continue;
        kept.push_back(match);
    }
    return kept;
}

}   // namespace MapViewportCandidates
