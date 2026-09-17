#pragma once

#include "Runtime/FramePacer.h"
#include <chrono>
#include <string>
#include <thread>
#include <vector>

// A thread that owns a low-level input hook is only allowed to block while it keeps servicing its
// message queue. These checks pin that behaviour down without needing a game or a real hook.
inline void TestFramePacer(void (*check)(bool, const std::string&)) {
    using namespace std::chrono_literals;
    FramePacer pacer;

    int pumps = 0;
    const auto start = FramePacer::Clock::now();
    const bool completed = pacer.WaitUntil(start + 40ms, [&]() { ++pumps; std::this_thread::sleep_for(1ms); return true; });
    const auto elapsed = FramePacer::Clock::now() - start;
    check(completed, "pumping wait reports normal completion");
    check(pumps >= 2, "pumping wait services the pump on entry and after the wait");
    check(elapsed >= 30ms, "pumping wait still honours its deadline");
    check(elapsed < 500ms, "pumping wait does not overshoot its deadline");

    // A stop request must end the wait immediately instead of sleeping out the frame.
    const auto stopping = FramePacer::Clock::now();
    const bool stopped = pacer.WaitUntil(stopping + 2s, []() { return false; });
    check(!stopped && FramePacer::Clock::now() - stopping < 500ms, "pumping wait stops as soon as the pump asks it to");

    // A deadline that already passed must service exactly one pump and return.
    pumps = 0;
    check(pacer.WaitUntil(FramePacer::Clock::now() - 10ms, [&]() { ++pumps; return true; }), "an expired deadline still completes");
    check(pumps == 1, "an expired deadline performs a single pump");

    // Real input wakes the wait before its deadline. Waiting still lasts until the deadline, so the
    // property to check is that the pump ran again long before it.
    MSG probe{};
    ::PeekMessageW(&probe, nullptr, 0, 0, PM_NOREMOVE); // give this thread a message queue
    const DWORD owner = ::GetCurrentThreadId();
    std::thread poster([owner]() {
        std::this_thread::sleep_for(20ms);
        ::PostThreadMessageW(owner, WM_NULL, 0, 0);
    });
    std::vector<FramePacer::Clock::time_point> pumpTimes;
    const auto waiting = FramePacer::Clock::now();
    const bool woke = pacer.WaitUntil(waiting + 600ms, [&]() {
        pumpTimes.push_back(FramePacer::Clock::now());
        MSG message{};
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { }
        return true;
    });
    const auto wokeAfter = FramePacer::Clock::now() - waiting;
    poster.join();
    check(woke, "waiting with a posted message still completes normally");
    check(wokeAfter >= 500ms, "waiting with a posted message still honours its deadline");
    bool servicedEarly = false;
    for (const auto& at : pumpTimes) if (at - waiting < 300ms) servicedEarly = true;
    check(servicedEarly, "a queued message is serviced long before the deadline");
    check(pumpTimes.size() >= 2, "the early wake runs the pump again");
}
