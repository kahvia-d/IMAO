#pragma once
#include "App/VisualRecoveryHints.h"
#include "Coordinate/VisualLocalization/GlobalVisualLocalizer.h"
#include "Coordinate/VisualLocalization/CoarseSearchCoverage.h"
#include <array>

inline void TestVisualRecoveryHints(void (*check)(bool, const std::string&)) {
    using namespace std::chrono_literals;
    const auto start = VisualRecoveryHints::Clock::now();
    VisualRecoveryHints policy;
    policy.SetEpoch(1, 4);
    policy.Submitted(1, "1/cave/A", true);
    policy.Complete(1, false, true, false, start);
    check(!policy.AllowScope("1/cave/A", start + 10s), "complete scope failure stays revoked in this generation");
    check(!policy.AllowScope("1/cave/B", start + 1s), "fallback gets continuous global opportunity");
    check(policy.AllowScope("1/cave/B", start + 3s), "different scope remains available after global opportunity");
    policy.SetEpoch(1, 5);
    check(policy.AllowScope("1/cave/A", start), "new generation can retry failed scope");
    policy.Submitted(2, "1/cave/A", true);
    policy.Complete(1, false, true, false, start);
    check(policy.AllowScope("1/cave/A", start), "late old request cannot reject active scope");
    policy.Complete(2, true, true, false, start);
    check(!policy.AllowScope("1/cave/A", start + 1s) && policy.AllowScope("1/cave/A", start + 2s),
        "timeout gives global work time without permanently rejecting cave");
    policy.Submitted(4, "1/cave/A", true);
    policy.Complete(4, true, true, false, start + 2s);
    check(!policy.AllowScope("1/cave/A", start + 5s) && policy.AllowScope("1/cave/A", start + 6s),
        "repeated timeout expands global search opportunity instead of resetting every two seconds");
    policy.Submitted(3, {}, false);
    policy.Complete(3, false, false, true, start + 2s);
    check(policy.PreserveGlobal(start + 3s) && !policy.PreserveGlobal(start + 4s),
        "global candidate protects the existing two-second confirmation window");

    // Reproduce the observed empty-hint requests 2,4,7: each interrupted
    // request must retain the coverage the actual worker keeps by version.
    CoarseSearchCoverage coverage;
    const std::array<std::uint32_t, 4> ranks{0, 1, 2, 3};
    std::uint64_t previousVersion = 0;
    for (const auto version : {2u, 4u, 7u}) {
        VisualLocalizationRequest request;
        request.hintVersion = version;
        request.NormalizeHints();
        if (previousVersion != request.hintVersion) coverage.Reset();
        const auto page = coverage.BeginPage(4, ranks, 1);
        check(page.ranks.front() == (version == 2 ? 0 : version == 4 ? 1 : 2),
            "empty hint request sequence preserves search progress");
        coverage.RecordVerified(ranks[page.ranks.front()], false);
        coverage.InterruptPage();
        previousVersion = request.hintVersion;
    }
    VisualLocalizationRequest bounded;
    bounded.hintVersion = 8;
    bounded.ocrHints.push_back({1, {100, 200}});
    bounded.requireOcrHint = true;
    bounded.NormalizeHints();
    check(bounded.requireOcrHint && bounded.hintVersion == 8, "real bounded hint keeps its revision");
    bounded.ocrHints.clear();
    bounded.NormalizeHints();
    check(!bounded.requireOcrHint && bounded.hintVersion == 0, "cleared hint changes back to global identity");
}
