#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <stdexcept>

namespace OverlayUiScale {
enum class Part { MinimapStatus, MapStatus, Tools, Guide, Hint, MapMarker, MinimapMarker, HandPalette, HandNode, StatusBall, Count };
inline double Automatic(double dpi, double width, double height) {
    if (!std::isfinite(dpi) || dpi <= 0) dpi = 96;
    if (!std::isfinite(width) || width <= 0) width = 1920;
    if (!std::isfinite(height) || height <= 0) height = 1080;
    return std::clamp(std::max(dpi / 96, std::min(width / 1920, height / 1080)), 1.0, 4.0);
}
struct Settings {
    std::array<std::atomic<int>, static_cast<std::size_t>(Part::Count)> values;
    Settings() { for (auto& value : values) value.store(100, std::memory_order_relaxed); }
};
inline Settings settings;
inline std::size_t Index(Part part) {
    const auto index = static_cast<std::size_t>(part);
    if (index >= settings.values.size()) throw std::invalid_argument("Invalid overlay UI part");
    return index;
}
inline int Percent(Part part) { return settings.values[Index(part)].load(std::memory_order_relaxed); }
inline void Apply(Part part, int percent) {
    if (percent < 50 || percent > 200) throw std::invalid_argument("Overlay UI percent must be 50-200");
    settings.values[Index(part)].store(percent, std::memory_order_relaxed);
}
inline double Effective(Part part, double dpi, double width, double height) {
    return Automatic(dpi,width,height) * Percent(part) / 100.0;
}
// Published by the render thread from the game client, never from the cropped overlay window.
// Other producers (minimap snapshots) read the same automatic factor atomically.
inline std::atomic<double> frameAutomatic{1};
inline void SetFrame(double dpi, double width, double height) { frameAutomatic.store(Automatic(dpi,width,height),std::memory_order_relaxed); }
inline float Frame(Part part) { return static_cast<float>(frameAutomatic.load(std::memory_order_relaxed)*Percent(part)/100.0); }
inline float MarkerRadius(Part part) { return (part == Part::MinimapMarker ? 11.52f : 12.96f) * Frame(part); }
}
