#include "Runtime/LocalizationLatencyTrace.h"
#include <cstdlib>
#include <iostream>
#include <string>

using Trace = LocalizationLatencyTrace;
static Trace::Time At(int ms) { return Trace::Time{} + std::chrono::milliseconds(ms); }
static void Has(const std::string& text, const std::string& part) {
    if (text.find(part) == std::string::npos) {
        std::cerr << "Missing " << part << " in " << text << '\n';
        std::exit(1);
    }
}
int main() {
    Trace trace;
    trace.SetStage("search", 10, At(0));
    trace.SetStage("wait", 10, At(100));
    trace.SetStage("capture", 11, At(250));
    auto result = trace.Describe(At(0), At(281));
    Has(result, "totalMs=281.000"); Has(result, "coveredMs=281.000");
    Has(result, "uncoveredMs=0.000"); Has(result, "dominantStage=wait");
    Has(result, "search:100.000"); Has(result, "wait:150.000");
    Has(result, "capture:31.000"); Has(result, "capture@11:250.000-281.000");
    result = trace.Describe(At(90), At(260));
    Has(result, "totalMs=170.000"); Has(result, "search:10.000");
    Has(result, "capture:10.000");
    result = trace.Describe(At(300), At(200));
    Has(result, "totalMs=0.000"); Has(result, "coveredMs=0.000");
    Trace late;
    late.SetStage("later", 1, At(100));
    Has(late.Describe(At(0), At(50)), "uncoveredMs=50.000");
    Has(late.Describe(At(0), At(150)), "uncoveredMs=100.000");
    Trace repeated;
    repeated.SetStage("work", 1, At(0));
    repeated.SetStage("wait", 1, At(20));
    repeated.SetStage("work", 2, At(30));
    Has(repeated.Describe(At(0), At(60)), "work:50.000");
    Has(repeated.Describe(At(0), At(10)), "work:10.000");
    repeated.SetStage("backdated", 3, At(25));
    Has(repeated.Describe(At(0), At(60)), "coveredMs=60.000");
    Has(repeated.Describe(At(0), At(60)), "backdated:30.000");
    Trace ring;
    for (int i = 0; i <= 300; ++i) ring.SetStage("work", i, At(i));
    result = ring.Describe(At(0), At(301));
    Has(result, "coveredMs=257.000"); Has(result, "uncoveredMs=44.000");
    Has(result, "historyTruncated=1"); Has(result, "work@300:300.000-301.000");
    Has(ring.Describe(At(290), At(301)), "historyTruncated=0");
    if (result.size() > 8192 || result.find("work@44:") != std::string::npos) return 2;
    Trace nested;
    nested.SetStage("outer", 1);
    {
        Trace::Scope scope(nested, "middle", 2);
        { Trace::Scope inner(nested, "inner", 3); }
        const auto start = Trace::Clock::now();
        Has(nested.Describe(start, start + std::chrono::milliseconds(1)), "dominantStage=middle");
        Has(nested.Describe(start, start + std::chrono::milliseconds(1)), "middle@2:");
    }
    const auto start = Trace::Clock::now();
    Has(nested.Describe(start, start + std::chrono::milliseconds(1)), "dominantStage=outer");
    Has(nested.Describe(start, start + std::chrono::milliseconds(1)), "outer@1:");
    std::cout << "LocalizationLatencyTraceTests passed\n";
}
