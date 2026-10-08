#pragma once

// The query side of the visual index, in one place for both localizers.
//
// The minimap asks "where am I" by ranking the index's tiles and verifying a handful of
// them. The full-screen map answers the same question, but used to compare its viewport
// against *every* tile of *every* approved scene instead - nine candidate copies of 6-19 MB
// each, at four zoom factors and two crops (see MapViewportLocalizer). The ranking is the
// same work in both cases, so it lives here once: the minimap's coarse stage and the
// full-screen map's cold-start hint cannot drift apart, and a change to the weighting is
// then a change to both by construction.
//
// What a caller does with the ranking stays at the call site. How many candidates it
// verifies, which scenes it keeps, and whether it treats the top entry as a position at all
// are three different policies that happen to share one scoring rule.

#include "MapVisualIndex.h"
#include "../RuntimeFeatureRepository.h"

#include <opencv2/core.hpp>
#include <opencv2/flann.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

namespace VisualIndexRetrieval {

// The vocabulary search every query is measured against. Building it belongs to whoever owns
// the index rather than to a query: the matrix is 4096x128 floats and the KD-tree over it is
// trained once. Returns an empty pointer when the vocabulary is not the shape the index
// describes, which is how a caller learns that this index cannot rank anything.
inline cv::Ptr<cv::flann::Index> BuildVocabularyIndex(const cv::Mat& vocabulary) {
    if (vocabulary.empty() || vocabulary.type() != CV_32F ||
        vocabulary.rows != static_cast<int>(MapVisualIndex::WordCount) ||
        vocabulary.cols != static_cast<int>(MapVisualIndex::DescriptorColumns)) {
        return {};
    }
    return cv::makePtr<cv::flann::Index>(vocabulary, cv::flann::KDTreeIndexParams(4));
}

// One TF-IDF score per tile, accumulated over the posting lists of the words the query hit.
//
// Returns false when the query produced no usable weight at all. That is the caller's signal
// that it has nothing to rank and must behave as if the index were not there - a query whose
// descriptors miss every visual word is not evidence that nowhere matches.
inline bool ScoreTiles(const RuntimeFeatureResources& resources, cv::flann::Index& vocabularyIndex,
    const cv::Mat& descriptors, std::vector<double>& scores) {
    scores.clear();
    const auto& index = resources.visualIndex;
    if (descriptors.empty() || descriptors.type() != CV_32FC1 ||
        descriptors.cols != static_cast<int>(MapVisualIndex::DescriptorColumns) ||
        index.tiles.empty() || index.postingOffsets.size() < MapVisualIndex::WordCount + 1) {
        return false;
    }
    cv::Mat words(descriptors.rows, 1, CV_32S);
    cv::Mat distances(descriptors.rows, 1, CV_32F);
    vocabularyIndex.knnSearch(descriptors, words, distances, 1, cv::flann::SearchParams(64));
    std::vector<std::uint32_t> counts(MapVisualIndex::WordCount, 0);
    for (int row = 0; row < words.rows; ++row) {
        const int word = words.at<int>(row);
        if (word >= 0 && word < static_cast<int>(counts.size())) ++counts[word];
    }
    std::vector<double> queryWeights(MapVisualIndex::WordCount, 0.0);
    double normSquared = 0.0;
    const double tileCount = static_cast<double>(index.tiles.size());
    for (std::uint32_t word = 0; word < MapVisualIndex::WordCount; ++word) {
        if (counts[word] == 0) continue;
        const auto begin = index.postingOffsets[word];
        const auto end = index.postingOffsets[word + 1];
        const double idf = std::log((tileCount + 1.0) / (static_cast<double>(end - begin) + 1.0)) + 1.0;
        const double tf = static_cast<double>(counts[word]) / descriptors.rows;
        queryWeights[word] = tf * idf;
        normSquared += queryWeights[word] * queryWeights[word];
    }
    if (!(normSquared > 0.0)) return false;
    const double inverseNorm = 1.0 / std::sqrt(normSquared);
    scores.assign(index.tiles.size(), 0.0);
    for (std::uint32_t word = 0; word < MapVisualIndex::WordCount; ++word) {
        const double queryWeight = queryWeights[word] * inverseNorm;
        if (!(queryWeight > 0.0)) continue;
        const auto begin = index.postingOffsets[word];
        const auto end = index.postingOffsets[word + 1];
        for (std::uint32_t postingIndex = begin; postingIndex < end; ++postingIndex) {
            // A truncated or hand-built index must rank what it has rather than read past it.
            if (postingIndex >= index.postings.size()) break;
            const auto& posting = index.postings[postingIndex];
            if (posting.tileIndex >= scores.size()) continue;
            scores[posting.tileIndex] += queryWeight * posting.weight;
        }
    }
    return true;
}

}  // namespace VisualIndexRetrieval
