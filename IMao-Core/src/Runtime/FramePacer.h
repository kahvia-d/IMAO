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

    // Windows delivers a low-level input hook to the thread that installed it and waits for that
    // thread to run the callback, so a thread owning WH_MOUSE_LL / WH_KEYBOARD_LL must never sleep
    // through a whole frame: every millisecond spent in a blind wait is added to the latency of the
    // player's mouse and keyboard. This overload pumps messages while it waits and returns false as
    // soon as pump() reports that the owner wants to stop.
    template <typename Pump>
    bool WaitUntil(Clock::time_point deadline, Pump&& pump) {
        for (;;) {
            if (!pump()) return false;
            const auto remaining = deadline - Clock::now();
            if (remaining <= Clock::duration::zero()) return true;
            if (!WaitForInputOrDeadline(remaining)) {
                // A failed wait must never become a busy loop; the next iteration pumps again.
                std::this_thread::sleep_for(std::min(remaining,
                    std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(2))));
            }
        }
    }

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
    // Wakes on the deadline or on any message queued for this thread. MWMO_INPUTAVAILABLE also
    // reports input that arrived before the call, so a hook callback can never be missed here.
    bool WaitForInputOrDeadline(Clock::duration remaining) {
        if (!timer_) return false;
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<LONGLONG>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() / 100);
        if (!SetWaitableTimer(timer_.get(), &due, 0, nullptr, nullptr, FALSE)) return false;
        const HANDLE handles[] = { timer_.get() };
        return MsgWaitForMultipleObjectsEx(1, handles, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE) != WAIT_FAILED;
    }

    wil::unique_handle timer_;
};
