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
// Splitting the overlay's own frame work, because measuring all of it as one piece says what it costs
// but not which part of it does. The clear is separable at all only because the clear colour is fully
// transparent: every pixel the draw data does not cover stays black, and black is the colorkey, so
// skipping the clear cannot change what the player sees.
inline constexpr int kOverlayClear = 1 << 4;
inline constexpr int kWindowSync = 1 << 5;
// Not a "switch off" like the others: this one changes how the overlay is presented, from a colorkey
// layered window over a blt-model swap chain to a DirectComposition visual over a flip-model one. It
// is the leading explanation for why an equivalent probe window costs 1.8 fps while the real overlay
// costs 13-16 fps (a blt-model surface needs an extra copy from DWM), and it is measured by comparing
// the baseline against the baseline with this bit set. Applies when the overlay session starts.
inline constexpr int kOverlayComposition = 1 << 6;
// Not a "switch off", and no longer how the small window is enabled: a window the size of the minimap
// is now the ordinary minimap state, measured at about +9.6 fps with p95 down 2.4 ms and the over-20 ms
// share at zero (Docs/MiniMapOverlayDesign_zh-Hans.md). What this bit selects is the other status-bar
// style - the full bar reaches across the client, so the window grows to hold the union of the two.
// Unset is the minimal style: the same small window, with the state shown as one coloured ball.
inline constexpr int kFullStatusBar = 1 << 7;
// Keeps the comparison the small window was decided by available after it became the default: with this
// bit the overlay goes back to a window the size of the whole game client, so "full client against
// minimap window" can be re-measured on any scene without rebuilding, and it doubles as the rollback.
inline constexpr int kForceFullOverlay = 1 << 8;
inline constexpr int kAll = kCapture | kGameStateDetection | kLocalization | kOverlayRender |
    kOverlayClear | kWindowSync | kOverlayComposition | kFullStatusBar | kForceFullOverlay;

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
    case kOverlayClear: return "关闭覆盖层整屏清屏";
    case kWindowSync: return "关闭窗口几何同步";
    case kOverlayComposition: return "改用 DirectComposition 呈现";
    case kFullStatusBar: return "完整状态栏模式（并集窗口）";
    case kForceFullOverlay: return "强制整屏覆盖层（对照/回滚）";
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
    case kOverlayClear: return "no-overlay-clear";
    case kWindowSync: return "no-window-sync";
    case kOverlayComposition: return "overlay-composition";
    case kFullStatusBar: return "full-status-bar";
    case kForceFullOverlay: return "force-full-overlay";
    default: return "custom";
    }
}

/// True when the overlay should present through DirectComposition instead of a colorkey window.
inline bool UseOverlayComposition() {
    return Enabled(kOverlayComposition);
}
}
