#pragma once

#include "Coordinate/VisualLocalization/CoarseSearchCoverage.h"

#include <algorithm>
#include <array>
#include <numeric>
#include <string>
#include <vector>

inline void TestCoarseSearchCoverage(void (*check)(bool, const std::string&)) {
    CoarseSearchCoverage coverage;
    const std::array<std::uint32_t, 6> original{ 0, 1, 2, 3, 4, 5 };
    auto page = coverage.BeginPage(6, original, 3);
    check(page.startsNewCycle && page.ranks == std::vector<std::size_t>{ 0, 1, 2 },
        "first recovery page starts a cycle and uses the best ranked regions");
    coverage.RecordVerified(0, false);
    coverage.RecordVerified(1, true);
    // Region 1 matched, but region 2 exhausted the budget before returning.
    coverage.InterruptPage();
    check(coverage.WasVisited(0) && !coverage.WasVisited(1) && !coverage.WasVisited(2),
        "budget interruption preserves failed-region work while retrying the discarded candidate and unfinished region");

    const std::array<std::uint32_t, 6> reordered{ 2, 0, 4, 1, 3, 5 };
    page = coverage.BeginPage(6, reordered, 3);
    check(!page.startsNewCycle && page.ranks == std::vector<std::size_t>{ 0, 2, 3 },
        "a candidate moving across page boundaries remains eligible by stable region identity");
    check(coverage.NextRank(reordered) == 0,
        "next-region diagnostics point at the unfinished region after rollback");
    for (const auto rank : page.ranks) coverage.RecordVerified(reordered[rank], reordered[rank] == 1);
    coverage.CompletePage();
    check(coverage.WasVisited(1), "a complete page keeps its returned candidate covered");
    page = coverage.BeginPage(6, reordered, 3);
    check(page.ranks == std::vector<std::size_t>{ 4, 5 },
        "continuation inspects remaining regions instead of repeating already verified copies");
    for (const auto rank : page.ranks) coverage.RecordVerified(reordered[rank], false);
    coverage.CompletePage();
    page = coverage.BeginPage(6, reordered, 3);
    check(page.startsNewCycle && page.ranks == std::vector<std::size_t>{ 0, 1, 2 },
        "finishing a full recovery pass restarts coverage and permits one new full-shard fallback");

    coverage.RecordVerified(2, true);
    page = coverage.BeginPage(6, reordered, 3);
    check(!coverage.WasVisited(2) && !page.ranks.empty() && reordered[page.ranks.front()] == 2,
        "an abandoned page cannot silently consume an unpublished geometric candidate");
    coverage.RecordVerified(2, false);
    coverage.CompletePage();
    const std::array<std::uint32_t, 0> empty{};
    page = coverage.BeginPage(6, empty);
    check(page.ranks.empty() && coverage.WasVisited(2),
        "a featureless ranking does not erase completed work from the recovery pass");

    auto cancelledRequest = coverage;
    cancelledRequest.BeginPage(6, reordered, 3);
    cancelledRequest.RecordVerified(1, false);
    cancelledRequest.CompletePage();
    check(!coverage.WasVisited(1) && cancelledRequest.WasVisited(1),
        "request-local coverage changes remain isolated until the worker accepts that request");
    coverage.Reset();
    page = coverage.BeginPage(6, reordered, 3);
    check(page.startsNewCycle && !coverage.WasVisited(2),
        "cancellation or a new UI session starts with fresh recovery coverage");

    // A rank-offset cursor can skip regions forever when rankings change.
    // Stable IDs must instead verify all 30 regions once within six pages.
    CoarseSearchCoverage moving;
    std::array<std::uint32_t, 30> ranking{};
    std::iota(ranking.begin(), ranking.end(), 0U);
    std::array<int, 30> visits{};
    for (int pageIndex = 0; pageIndex < 6; ++pageIndex) {
        std::rotate(ranking.begin(), ranking.begin() + 7, ranking.end());
        const auto selected = moving.BeginPage(ranking.size(), ranking, 5);
        for (const auto rank : selected.ranks) {
            ++visits[ranking[rank]];
            moving.RecordVerified(ranking[rank], false);
        }
        moving.CompletePage();
    }
    check(std::all_of(visits.begin(), visits.end(), [](auto count) { return count == 1; }),
        "moving rankings cannot starve lower ranked base-map regions or repeatedly consume covered regions");
}
