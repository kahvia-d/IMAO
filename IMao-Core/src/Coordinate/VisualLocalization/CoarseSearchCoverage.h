#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

// One recovery pass covers stable region IDs, independent of each frame's
// ranking. A copy belongs to one request and is committed by the worker only
// while that request's cancellation epoch is still current.
class CoarseSearchCoverage {
public:
    struct Page {
        // Indices into the supplied ranking, so callers retain their scores.
        std::vector<std::size_t> ranks;
        bool startsNewCycle = false;
    };

    void Reset() {
        visited_.clear();
        candidateRegions_.clear();
    }

    bool WasVisited(std::uint32_t region) const {
        return region < visited_.size() && visited_[region] != 0;
    }

    Page BeginPage(std::size_t regionCount, std::span<const std::uint32_t> rankedRegions,
        std::size_t maximumRegions = 12) {
        // Abandoning a previous page must not silently consume an unpublished
        // candidate. Normal callers explicitly CompletePage/InterruptPage.
        InterruptPage();
        if (visited_.size() != regionCount) visited_.assign(regionCount, 0);
        for (const auto region : rankedRegions) {
            if (region >= visited_.size()) throw std::out_of_range("coarse region ID exceeds resource size");
        }
        Page page;
        page.startsNewCycle = std::none_of(visited_.begin(), visited_.end(), [](auto value) { return value != 0; });
        if (!rankedRegions.empty() && std::all_of(rankedRegions.begin(), rankedRegions.end(),
            [&](auto region) { return WasVisited(region); })) {
            std::fill(visited_.begin(), visited_.end(), 0);
            page.startsNewCycle = true;
        }
        for (std::size_t rank = 0; rank < rankedRegions.size() && page.ranks.size() < maximumRegions; ++rank) {
            if (!WasVisited(rankedRegions[rank])) page.ranks.push_back(rank);
        }
        return page;
    }

    // Call only after a complete geometric verification. Merely selecting or
    // starting a region does not consume it when the time budget expires.
    void RecordVerified(std::uint32_t region, bool hasCandidate) {
        if (region >= visited_.size()) throw std::out_of_range("verified coarse region ID exceeds resource size");
        visited_[region] = 1;
        if (hasCandidate) candidateRegions_.push_back(region);
    }

    void CompletePage() { candidateRegions_.clear(); }

    void InterruptPage() {
        // Failed regions retain useful progress. Candidates discarded with an
        // incomplete page must be examined again on a later captured image.
        for (const auto region : candidateRegions_) visited_[region] = 0;
        candidateRegions_.clear();
    }

    std::size_t NextRank(std::span<const std::uint32_t> rankedRegions) const {
        for (std::size_t rank = 0; rank < rankedRegions.size(); ++rank) {
            if (!WasVisited(rankedRegions[rank])) return rank;
        }
        return 0;
    }

private:
    std::vector<std::uint8_t> visited_;
    std::vector<std::uint32_t> candidateRegions_;
};
