#pragma once
#include <atomic>
#include <cstdlib>
#include <cstdint>

namespace MapGpuSettings {
inline bool LaunchEnabled(const char* name) {
    char value[4]{};
    size_t size = 0;
    return getenv_s(&size, value, sizeof(value), name) == 0 && size == 2 && value[0] == '1';
}

// Initialized lazily so standalone replay tools retain their launch overrides.
inline std::atomic<int>& Mask() {
    static std::atomic<int> mask{(LaunchEnabled("IMAO_MINIMAP_GPU_RECOVERY") ? 1 : 0) |
                                 (LaunchEnabled("IMAO_VIEWPORT_GPU") ? 2 : 0)};
    return mask;
}
inline bool MinimapEnabled() { return (Mask().load() & 1) != 0; }
inline bool ViewportEnabled() { return (Mask().load() & 2) != 0; }
// Map-only observation trials leave the coordinate recognizer on its player-selected backend.
inline std::atomic<int> ocrOverride{-1};

// Eligible work and actual dispatches are separate: a quiet minute with the switch
// on, or a driver fallback, must not count as GPU-assisted recognition.
inline std::atomic<uint64_t> minimapBatches{}, minimapGpuBatches{};
inline std::atomic<uint64_t> viewportExtractions{}, viewportGpuExtractions{};
}
