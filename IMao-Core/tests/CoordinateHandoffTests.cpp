#include "Coordinate/IdentifyWorldCoordinates/CoordinateHandoff.h"
#include <iostream>
#include <stdexcept>
using namespace std::chrono_literals;
static void TestTrustedReadoutRecovery() {
    using H=CoordinateHandoff;const auto t=H::Clock::time_point{}+10s;
    auto check=[](bool b,const char* m){if(!b)throw std::runtime_error(m);};
    H h;H::ReadoutPrior prior{true,1,10,t,-1200,-6700};
    h.ObserveReadout(t);h.BreakReadoutStreak();
    check(h.ConfirmReadout(12,t+700ms,1,-1208,-6703,&prior),
        "recent committed position must corroborate the first valid readout after a short blank");
    check(!h.ConfirmReadout(12,t+700ms,1,-1208,-6703,&prior),"same frame must never count twice");
    auto reject=[&](H::ReadoutPrior p,int scene,double x,double y,const char* m) {
        H cold;check(!cold.ConfirmReadout(12,t+700ms,scene,x,y,&p),m);
    };
    auto missing=prior;missing.valid=false;reject(missing,1,-1208,-6703,"uncommitted prior must still require two new frames");
    auto stale=prior;stale.capturedAt=t-1s;reject(stale,1,-1208,-6703,"old position cannot bypass initial confirmation");
    reject(prior,2,-1208,-6703,"another region must not corroborate a readout");
    reject(prior,1,1208,-6703,"lost minus sign must not be corroborated by history");
    reject(prior,1,-1500,-6703,"impossible displacement must still wait for independent validation");
    auto future=prior;future.capturedAt=t+701ms;reject(future,1,-1208,-6703,"future prior must not corroborate this reading");
    auto duplicate=prior;duplicate.frame=12;reject(duplicate,1,-1208,-6703,"a prior from this very frame cannot be an independent observation");
    H initial;check(!initial.ConfirmReadout(1,t,1,10,20),"initial readout must wait without a trusted prior");
    initial.BreakReadoutStreak();check(!initial.ConfirmReadout(2,t+200ms,1,11,20),"a blank before initial acquisition must still break the streak");
    check(initial.ConfirmReadout(3,t+400ms,1,12,20),"two actual initial readings still acquire normally");
}
int main() {
    try {
    TestTrustedReadoutRecovery();
    {
        CoordinateHandoff active;const auto at=CoordinateHandoff::Clock::time_point{}+10s;
        active.ObserveReadout(at);
        if(active.Cadence(at+10ms)!=150ms)throw std::runtime_error("active readout must use the worker's 150ms cadence instead of250ms");
        if(active.ObserveVisual(1,at+100ms,1,1)||!active.ObserveVisual(2,at+200ms,1,1))
            throw std::runtime_error("cadence fixture must return through real visual corroboration");
        if(active.Cadence(at+201ms)!=1000ms)throw std::runtime_error("healthy SURF must retain low-frequency standby");
    }
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
