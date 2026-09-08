#include "GamepadCursorTargetsTests.h"
#include <iostream>
int main() {
    int checks = 0, failures = 0;
    TestGamepadCursorTargets([&](bool condition, const std::string& message) {
        ++checks; if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    });
    std::cout << "Gamepad cursor targets: " << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
