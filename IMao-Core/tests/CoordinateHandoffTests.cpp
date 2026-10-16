#include "Coordinate/IdentifyWorldCoordinates/CoordinateHandoff.h"
#include <iostream>
#include <stdexcept>
using namespace std::chrono_literals;
int main() {
    try {
    using H = CoordinateHandoff;
    auto t = H::Clock::time_point{} + 10s;
    auto require=[](bool b) { if (!b) throw std::runtime_error("handoff invariant failed"); };
    H h;
    require(h.NeedsReadout(t));
    require(!h.FreshReadout(1,t,t+751ms));
    require(!h.FreshReadout(1,t+1ms,t));
    require(h.FreshReadout(1,t,t+20ms));
    require(!h.ConfirmReadout(1,t,1,10,20));
    require(!h.ConfirmReadout(1,t,1,10,20));
    require(h.ConfirmReadout(2,t+250ms,1,20,20));
    require(!h.ConfirmReadout(3,t+500ms,2,20,20));
    require(!h.ConfirmReadout(4,t+3s,2,20,20));
    h.Reset();
    require(h.ObserveVisual(1, t, 100, 100));
    require(!h.NeedsReadout(t));
    require(h.Cadence(t) == 1000ms);
    h.ObserveFailure();
    require(h.NeedsReadout(t + 10ms));
    require(h.Cadence(t + 10ms) == 250ms);
    h.ObserveReadout(t + 20ms);
    require(h.NeedsReadout(t + 30ms)); // OCR cannot heal SURF failures.
    require(!h.ObserveVisual(2, t + 40ms, 102, 100));
    require(h.ReadoutActive());
    require(!h.ObserveVisual(2, t + 50ms, 102, 100)); // Same frame is not corroboration.
    require(!h.ObserveVisual(9, t + 40ms, 102, 100)); // Same capture time is not a new observation.
    h.ObserveFailure(); // A failed local attempt must not erase independent global recovery evidence.
    require(h.ObserveVisual(3, t + 100ms, 104, 100));
    require(!h.ReadoutActive());
    require(!h.NeedsReadout(t + 100ms));
    require(h.NeedsReadout(t + 701ms)); // Relative tracking cannot keep SURF healthy.
    h.ObserveReadout(t + 710ms);
    require(!h.ObserveVisual(4, t + 720ms, 105, 100));
    require(!h.ObserveVisual(5, t + 730ms, 1000, 100)); // Inconsistent recovery restarts streak.
    h.Reset();
    require(!h.ReadoutActive());
    require(h.NeedsReadout(t));
    std::cout << "Coordinate handoff tests passed\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
