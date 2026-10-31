#include "../src/Runtime/PositionDisplayPolicy.h"
#include <iostream>
int main() {
    using namespace std::chrono;
    const auto captured = steady_clock::time_point(seconds(10));
    const auto now = captured + seconds(30);
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { std::cerr << "FAIL " << message << '\n'; ++failures; }
    };
    check(HasDisplayPosition(true,1,1,4,4,captured,now), "same-context position persists through missing recognition");
    check(!HasDisplayPosition(true,1,2,4,4,captured,now), "scene change revokes position");
    check(!HasDisplayPosition(true,1,1,4,5,captured,now), "context change revokes position");
    check(!HasDisplayPosition(false,1,1,4,4,captured,now), "invalidated position cannot return");
    check(!HasDisplayPosition(true,1,1,4,4,now+seconds(1),now), "future position rejected");
    check(!HasDisplayPosition(true,1,1,4,4,{},now), "uninitialized position rejected");
    std::cout << "Position display failures=" << failures << '\n';
    return failures ? 1 : 0;
}
