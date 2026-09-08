#include "MapToolsTests.h"
#include <iostream>
int main() {
    int count = 0, failures = 0;
    TestMapTools([&](bool condition, const std::string& message) {
        ++count; if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    });
    std::cout << "Map tools and overlay restart: " << count << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
