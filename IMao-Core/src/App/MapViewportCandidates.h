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

// The alternative: match every chosen row against the query and keep the pairs that pass the same ratio
// test, reporting match indices against the *row list* rather than against a copy. Nothing is allocated
// beyond the result vectors and the pair list, and de-duplication happens on the pairs, where it costs a
// hash insert per surviving match instead of one per candidate row.
inline std::vector<cv::DMatch> MatchRowsExactly(const RuntimeFeatureResources& resources,
    const std::vector<std::uint32_t>& rows, const cv::Mat& queryDescriptors,
    const std::function<bool()>& interrupted = {}) {
    std::vector<cv::DMatch> good;
    if (rows.empty() || queryDescriptors.empty()) return good;
    // MatchDescriptorsExactly reports its first argument by row index, and its first argument is the query,
    // so a pair that relates query descriptor `trainIdx` to map row `queryIdx` is exactly the relation this
    // path needs. Getting those two the wrong way round is a crash, not a wrong answer: the indices are then
    // used against arrays sized the other way (2026-10-02).
    std::vector<std::vector<cv::DMatch>> forward;
    std::vector<std::vector<cv::DMatch>> reverse;
    MatchDescriptorsExactly(queryDescriptors, resources.map.imgDescriptors, forward, reverse, interrupted);
    good.reserve(rows.size());
    for (std::size_t position = 0; position < rows.size(); ++position) {
        const std::size_t row = rows[position];
        if (row >= forward.size()) continue;
        const auto& pairs = forward[row];
        if (pairs.size() < 2 || pairs[0].distance >= 0.62f * pairs[1].distance ||
            pairs[0].distance > 0.50f) continue;
        // trainIdx is a row of the second argument, which here is the map: that is the map row this query
        // descriptor chose, and it has to be the row being considered.
        if (pairs[0].trainIdx < 0 || static_cast<std::size_t>(pairs[0].trainIdx) != row) continue;
        // Mutual: the crop descriptor's own nearest row must also be this one. The windowed path gets this
        // from matching one query against a de-duplicated candidate set, where a shared second choice can
        // only cost a match; matching the whole map row set makes the check explicit.
        const int queryIndex = pairs[0].queryIdx;
        if (queryIndex < 0 || queryIndex >= static_cast<int>(reverse.size())) continue;
        if (reverse[queryIndex].empty() || reverse[queryIndex][0].trainIdx != static_cast<int>(row)) continue;
        good.emplace_back(static_cast<int>(position), queryIndex, pairs[0].distance);
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
