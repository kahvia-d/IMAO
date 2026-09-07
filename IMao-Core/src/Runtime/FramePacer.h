#pragma once
#include <Windows.h>
#include <chrono>
#include <thread>
#include <algorithm>
#include <wil/resource.h>

class FramePacer {
public:
    using Clock = std::chrono::steady_clock;
    FramePacer() : timer_(CreateWaitableTimerExW(nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)) {}
    void WaitUntil(Clock::time_point deadline) {
        const auto remaining = deadline - Clock::now();
        if (remaining <= Clock::duration::zero()) return;
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<LONGLONG>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() / 100);
        if (timer_ && SetWaitableTimer(timer_.get(), &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer_.get(), 100);
        } else std::this_thread::sleep_until(deadline);
    }
private:
    wil::unique_handle timer_;
};
