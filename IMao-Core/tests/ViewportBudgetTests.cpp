#include <cstdint>
#include <iostream>
#include <vector>
#include "App/MapViewportWorkBudget.h"
int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* name) { if (!ok) { std::cerr << "FAIL: " << name << '\n'; ++failures; } };
    MapViewportWorkBudget budget;
    std::vector<int> visited(16);
    for (int request = 0; request < 32; ++request) {
        auto group = budget.Begin(1, 2, 3, visited.size());
        ++visited[group];
        budget.Completed(group);
        check(MapViewportWorkBudget::ShouldYield(true, 1, 2001), "global yields after a complete group exceeds budget");
    }
    for (auto visits : visited) check(visits == 2, "repeated budget exhaustion does not starve later groups");
    budget.Completed(4);
    check(budget.Begin(1, 2, 3, 16) == 5, "continuation remembers only next work position");
    check(budget.Begin(2, 2, 3, 16) == 0, "new session resets schedule");
    budget.Completed(4);
    check(budget.Begin(2, 4, 3, 16) == 0, "new UI generation resets schedule");
    budget.Completed(4);
    check(budget.Begin(2, 4, 5, 16) == 0, "new viewport generation resets schedule");
    budget.Completed(4);
    check(budget.Begin(2, 4, 5, 8) == 0, "changed group layout resets schedule");
    check(!MapViewportWorkBudget::ShouldYield(false, 8, 9000), "local search bypasses global budget");
    check(!MapViewportWorkBudget::ShouldYield(true, 0, 9000), "at least one full ambiguity group always runs");
    check(!MapViewportWorkBudget::ShouldYield(true, 2, 1999), "global can continue while within budget");
    budget.Reset();
    check(budget.Begin(2, 4, 5, 8) == 0, "accepted result resets schedule");
    if (!failures) std::cout << "All viewport budget tests passed.\n";
    return failures ? 1 : 0;
}
