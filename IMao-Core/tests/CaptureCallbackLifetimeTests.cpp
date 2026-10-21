#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#ifdef IMAO_CAPTURE_UNSAFE_REFERENCE
// Original ownership contract: the event retains a raw owner; Close neither
// rejects queued callbacks nor drains an active callback before member release.
template<class Owner> class CaptureCallbackGate {
    Owner* owner_;
public:
    explicit CaptureCallbackGate(Owner* owner):owner_(owner){}
    template<class Action> bool Invoke(Action&& action){action(*owner_);return true;}
    void StopAndWait(){}
};
#else
#include "WindowsCapture/WindowsGraphicsCapture/CaptureCallbackGate.h"
#endif
using namespace std::chrono_literals;
struct Owner {std::atomic<bool> resourcesReleased{false};std::atomic<int> calls{0};};
int main(){int failures=0;const auto check=[&](bool ok,const char*why){if(!ok){++failures;std::cerr<<"FAIL "<<why<<'\n';}};
    // Deterministic destruction boundary: keep the marker object allocated so
    // the unsafe reference fails assertions instead of executing undefined C++.
    {
        Owner owner;auto gate=std::make_shared<CaptureCallbackGate<Owner>>(&owner);
        std::promise<void> entered,finishCallback,closing,closed;
        auto enteredFuture=entered.get_future();auto finishFuture=finishCallback.get_future().share();auto closingFuture=closing.get_future();auto closedFuture=closed.get_future();
        std::atomic<bool> usedReleasedResource{false};
        std::jthread callback([&]{gate->Invoke([&](Owner&o){entered.set_value();finishFuture.wait();usedReleasedResource=o.resourcesReleased.load();++o.calls;});});
        enteredFuture.wait();
        std::jthread closer([&]{closing.set_value();gate->StopAndWait();owner.resourcesReleased=true;closed.set_value();});
        closingFuture.wait();
        check(closedFuture.wait_for(50ms)==std::future_status::timeout,"close must drain an already active callback before resource release");
        finishCallback.set_value();callback.join();closer.join();
        check(!usedReleasedResource,"Unmap boundary must not see already released resources");
        check(owner.calls==1,"the in-flight frame completes exactly once");
        bool lateRan=false;check(!gate->Invoke([&](Owner&){lateRan=true;}),"queued callback must be rejected after close");
        check(!lateRan,"late callback must never access owner");
        gate->StopAndWait(); // Idempotence.
    }
    {
        auto owner=std::make_unique<Owner>();auto gate=std::make_shared<CaptureCallbackGate<Owner>>(owner.get());
        gate->StopAndWait();
#ifndef IMAO_CAPTURE_UNSAFE_REFERENCE
        owner.reset(); // The unsafe control keeps its marker alive to avoid UB.
#endif
        bool invoked=false;check(!gate->Invoke([&](Owner&){invoked=true;}),"queued event gate outlives owner without accessing it");
        check(!invoked,"destroyed owner is never handed to queued action");
    }
    {
        Owner owner;CaptureCallbackGate<Owner> gate(&owner);bool caught=false;
        try{gate.Invoke([](Owner&){throw std::runtime_error("frame failure");});}catch(const std::runtime_error&){caught=true;}
        check(caught,"callback exception propagates to frame error boundary");
        gate.StopAndWait();check(!gate.Invoke([](Owner&){}),"exception must release callback admission lock");
    }
    if(!failures)std::cout<<"All capture callback lifetime tests passed.\n";
    return failures?1:0;
}
