#include <iostream>
#include "OfflineClockBoundary.h"
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
int main() {
    try {
        Require(CoordinatePrefixBeforeClock("-965,1404,6 2026-10-1414:03:57+8* HD") == "-965,1404,6", "valid adjacent date/time rejected");
        Require(CoordinatePrefixBeforeClock("-536,-19,-27 2026-10-14 14:10:01+8*") == "-536,-19,-27", "signed coordinates changed");
        Require(!CoordinatePrefixBeforeClock("-1,2,32026-10-14 14:03:57"), "missing boundary must not invent z");
        Require(!CoordinatePrefixBeforeClock("-1,2,3,4 2026-10-14 14:03:57"), "fourth coordinate silently dropped");
        Require(!CoordinatePrefixBeforeClock("-1,2,3 2026-02-30 14:03:57"), "invalid calendar date accepted");
        Require(!CoordinatePrefixBeforeClock("-1,2,3 2026-10-14 25:03:57"), "invalid clock accepted");
        Require(!CoordinatePrefixBeforeClock("-1,2,3 2026-10-14"), "partial date must abstain");
        Require(!CoordinatePrefixBeforeClock("-1,2 2026-10-14 14:03:57"), "missing z fabricated");
        auto prefix = CoordinatePrefixBeforeClock("-2147483649,2,3 2026-10-14 14:03:57");
        Require(prefix && CoordinateCandidateParser::Parse(*prefix, .99f).empty(), "int32 gate changed");
        prefix = CoordinatePrefixBeforeClock("-1,2,3 2026-10-14 14:03:57");
        Require(prefix && CoordinateCandidateParser::Parse(*prefix, .64f).empty(), "confidence gate changed");
        std::cout << "Strict clock boundary contracts passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
