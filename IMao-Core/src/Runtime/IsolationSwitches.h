#pragma once
#include <atomic>

// Diagnostic only. The overlay's cost has been measured as a whole (about 26 fps with a long tail of
// frames over 20 ms) and attributed to "capture and localization" as a group, which is not actionable:
// that group contains several independent pieces of per-frame work with very different sizes.
//
// Each bit switches off one piece so its cost can be measured in isolation by difference against a
// baseline where everything runs. Nothing here changes behaviour when the mask is zero, and every
// piece stays disabled only for as long as a diagnostic asks for it.
//
// The pieces are chosen so that turning one off leaves the others measurable:
//   * capture off stops new pixels, which also stops the localization and tracking that consume them,
//     so it is the top of the dependency chain and is only ever switched off last.
//   * the overlay render bit keeps the window visible and presenting, so the cost it removes is the
//     frame building itself rather than the window's presence.
namespace Isolation {
inline constexpr int kCapture = 1 << 0;
inline constexpr int kGameStateDetection = 1 << 1;
inline constexpr int kLocalization = 1 << 2;
inline constexpr int kOverlayRender = 1 << 3;
inline constexpr int kAll = kCapture | kGameStateDetection | kLocalization | kOverlayRender;

inline std::atomic_int switches{ 0 };

inline bool Enabled(int bit) {
    return (switches.load(std::memory_order_relaxed) & bit) != 0;
}

inline int Set(int value) {
    switches.store(value & kAll, std::memory_order_relaxed);
    return switches.load(std::memory_order_relaxed);
}

inline const char* Describe(int value) {
    switch (value & kAll) {
    case 0: return "正常（全部开启）";
    case kCapture: return "关闭画面采集";
    case kGameStateDetection: return "关闭游戏状态检测";
    case kLocalization: return "关闭定位";
    case kOverlayRender: return "关闭覆盖层绘制";
    default: return "自定义组合";
    }
}

/// Japanese/Chinese-free name for logs and the measurement table.
inline const char* DescribeAscii(int value) {
    switch (value & kAll) {
    case 0: return "baseline-all-on";
    case kCapture: return "no-capture";
    case kGameStateDetection: return "no-game-state-detection";
    case kLocalization: return "no-localization";
    case kOverlayRender: return "no-overlay-render";
    default: return "custom";
    }
}
}
