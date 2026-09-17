#pragma once
#include <thread>
#include <windows.h>

// The tool's workers exist to serve the overlay, so they must never outrank the game they read frames
// from: a helper that competes with the game on equal terms is what makes the game drop frames.
// The overlay thread itself deliberately keeps normal priority, because it also services the input
// hooks, and a hook callback that waits for CPU is input latency the player feels.
namespace ThreadPriority {

inline void MakeBackground(std::thread& worker) {
    if (worker.joinable()) ::SetThreadPriority(worker.native_handle(), THREAD_PRIORITY_BELOW_NORMAL);
}

inline void MakeBackground(std::jthread& worker) {
    if (worker.joinable()) ::SetThreadPriority(worker.native_handle(), THREAD_PRIORITY_BELOW_NORMAL);
}
}
