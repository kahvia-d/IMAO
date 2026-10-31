#pragma once
#include <chrono>
#include <cstdint>

// A session-local accepted position is display state, not a new observation.
// Its original timestamp must remain unchanged for all non-display consumers.
inline bool HasDisplayPosition(bool valid, int scene, int currentScene,
    std::uint64_t generation, std::uint64_t currentGeneration,
    std::chrono::steady_clock::time_point capturedAt,
    std::chrono::steady_clock::time_point now) {
    return valid && scene > 0 && scene == currentScene && generation == currentGeneration &&
        capturedAt != std::chrono::steady_clock::time_point{} && now >= capturedAt;
}
