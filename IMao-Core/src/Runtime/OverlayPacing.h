#pragma once
#include <chrono>
#include <cstdint>

// Pacing and input-hook policy for the in-game overlay. These decisions are separated from the
// rendering code so they can be tested without a game window.
namespace OverlayPacing {
using Clock = std::chrono::steady_clock;

// Presenting a layered overlay window faster than the content changes only adds compositor pressure:
// the swap chain blocks until the compositor takes the frame, and that time is added to the latency of
// every input event the overlay thread also services. Marker positions change at the capture rate
// (about 30 Hz), so 30 Hz presentation keeps the same visible result with far less blocking.
inline constexpr std::chrono::microseconds kFramePeriod{33333};
inline constexpr std::chrono::microseconds kRouteCanvasFramePeriod{16667};

inline std::chrono::microseconds FramePeriod(bool routeCanvasActive) {
    return routeCanvasActive ? kRouteCanvasFramePeriod : kFramePeriod;
}

// The mouse hook decides synchronously whether a click belongs to the overlay, so it must be installed
// while the map publishes clickable regions. Installed for the whole session it would instead route
// every mouse event of the game through this frame-paced thread. The grace period covers a click that
// is already in flight when the map closes.
inline constexpr std::chrono::milliseconds kMouseHookGrace{500};

inline bool WantsMouseHook(bool mapInteractive, Clock::time_point regionsPublishedAt, Clock::time_point now,
    bool dragging = false, bool diagnosticDisabled = false) {
    // A drag that this hook already swallowed must keep owning its release, even when the map closes
    // or loses focus in between, so the game never receives a mouse-up without a matching down.
    if (dragging) return true;
    if (diagnosticDisabled) return false;
    return mapInteractive && regionsPublishedAt != Clock::time_point{} && now - regionsPublishedAt < kMouseHookGrace;
}

// Keep controller gestures separate from mouse down/up ownership during isolation.
struct MouseHookTransition {
    bool wanted = false;
    bool discardGestureState = true;
};

inline MouseHookTransition PlanMouseHook(bool mapInteractive, Clock::time_point regionsAt,
    Clock::time_point now, bool mouseButtonOwned, bool gestureActive, bool diagnosticDisabled) {
    return {WantsMouseHook(mapInteractive, regionsAt, now,
        mouseButtonOwned || (!diagnosticDisabled && gestureActive), diagnosticDisabled), !diagnosticDisabled};
}

// Capturing the game window is synchronous work inside the game, so it only follows the overlay rate
// while an overlay is actually attached; otherwise it runs at the recognition cadence, which is what
// the localization loop needs.
inline constexpr std::chrono::microseconds kCaptureActivePeriod{33333};
inline constexpr std::chrono::microseconds kCaptureIdlePeriod{80000};

// The fastest rate this process ever consumes captured frames. Windows Graphics Capture otherwise
// delivers every frame the game presents - on a 120 Hz game that is 120 full GPU readbacks plus a
// 14.7 MB copy per second for pixels nobody reads - while no consumer here asks for more than
// kCaptureActivePeriod. Telling the capture session this interval removes that work at the source,
// and it is deliberately a whole multiple of the display refresh so the frames we do get stay evenly
// spaced instead of arriving in pairs (measured: pairs of 100-176 ms game frames two seconds apart).
// It must never be larger than kCaptureActivePeriod, or the promise made above goes unmet.
inline constexpr std::chrono::microseconds kCaptureMinUpdateInterval{33333};

// A capture that took far longer than usual means the game is struggling to produce the extra frame
// this tool asks for. Waiting longer before asking again keeps such a stall from repeating back to
// back (measured: 116-176 ms stalls arrived in pairs two seconds apart).
inline constexpr double kSlowCaptureMs = 50.0;
inline constexpr std::chrono::microseconds kSlowCaptureBackoff{30000};

inline std::chrono::microseconds CapturePeriod(bool overlayActive) {
    return overlayActive ? kCaptureActivePeriod : kCaptureIdlePeriod;
}

inline std::chrono::microseconds CapturePeriod(bool overlayActive, bool slowCapture) {
    return CapturePeriod(overlayActive) + (slowCapture ? kSlowCaptureBackoff : std::chrono::microseconds::zero());
}

// 性能与刷新档位：统一协调采集帧率上限、大/小地图识别间隔、OCR 间隔与后台视觉探针频率。
enum class PerformanceProfile : int {
    Smooth = 0,       // 流畅优先
    Balanced = 1,     // 均衡（默认）
    LowOverhead = 2,  // 低占用（省帧率）
    Custom = 3        // 自定义
};

// 当前游戏运行场景：按场景动态切换 WGC 源头采集频率，避免 OCR 模式或空闲期白截 30 fps。
enum class CaptureScenario : int {
    Idle = 0,            // 未见地图 / 切后台 / 非探索界面
    MinimapReadout = 1,  // 小地图 + 坐标 OCR 读数模式（标点直接按 OCR 投影，无需高频像素追踪）
    MinimapVisual = 2,   // 小地图 + 纯视觉定位模式（需要连续画面做局部追踪）
    BigMap = 3           // 大地图打开（拖拽与缩放最吃标点跟手性）
};

inline const char* CaptureScenarioName(CaptureScenario scenario) {
    switch (scenario) {
    case CaptureScenario::BigMap: return "big-map";
    case CaptureScenario::MinimapVisual: return "minimap-visual";
    case CaptureScenario::MinimapReadout: return "minimap-readout";
    default: return "idle";
    }
}

inline const char* PerformanceProfileName(PerformanceProfile profile) {
    switch (profile) {
    case PerformanceProfile::Smooth: return "smooth";
    case PerformanceProfile::Balanced: return "balanced";
    case PerformanceProfile::LowOverhead: return "low-overhead";
    case PerformanceProfile::Custom: return "custom";
    default: return "balanced";
    }
}

struct PacingProfileSettings {
    int profile = static_cast<int>(PerformanceProfile::Balanced);
    int captureMaxFps = 20;
    int mapUpdateCycleMs = 80;
    int minMapUpdateCycleMs = 80;
    int ocrUpdateCycleMs = 150;
};

struct EffectivePacing {
    PerformanceProfile profile = PerformanceProfile::Balanced;
    std::chrono::microseconds bigMapCapturePeriod{50000};
    std::chrono::microseconds minimapVisualCapturePeriod{66667};
    std::chrono::microseconds minimapReadoutCapturePeriod{150000};
    std::chrono::microseconds idleCapturePeriod{125000};
    int mapUpdateCycleMs = 100;
    int minMapUpdateCycleMs = 100;
    std::chrono::milliseconds ocrSubmissionInterval{150};
    std::chrono::milliseconds readoutVisualProbeInterval{600};
};

inline std::chrono::microseconds PeriodFromFps(int fps) {
    const int clamped = fps < 5 ? 5 : (fps > 60 ? 60 : fps);
    return std::chrono::microseconds(1000000 / clamped);
}

inline int ClampMs(int value, int low, int high) {
    return value < low ? low : (value > high ? high : value);
}

// 根据档位与自定义设置解析当前生效的全链路调度参数。
inline EffectivePacing ResolveEffectivePacing(const PacingProfileSettings& settings) {
    const int rawProfile = settings.profile;
    const auto profile = (rawProfile >= 0 && rawProfile <= 3)
        ? static_cast<PerformanceProfile>(rawProfile)
        : PerformanceProfile::Balanced;

    switch (profile) {
    case PerformanceProfile::Smooth:
        return {
            profile,
            std::chrono::microseconds{33333},   // 大地图 30 fps（拖拽跟手）
            std::chrono::microseconds{33333},   // 小地图视觉 30 fps（高频像素粘合）
            std::chrono::microseconds{100000},  // OCR 模式对齐 100 ms（10 fps）
            std::chrono::microseconds{100000},  // 空闲巡检 10 fps
            80, 200,                            // 大地图 80 ms；小地图 SURF 绝对校准 200 ms
            std::chrono::milliseconds{100},
            std::chrono::milliseconds{300}
        };
    case PerformanceProfile::LowOverhead:
        return {
            profile,
            std::chrono::microseconds{83333},   // 大地图 12 fps（拖拽跟手）
            std::chrono::microseconds{100000},  // 小地图视觉 10 fps（轻量像素粘合）
            std::chrono::microseconds{200000},  // OCR 模式对齐 200 ms（5 fps）
            std::chrono::microseconds{150000},  // 空闲巡检 ~6.7 fps
            160, 250,                           // 大地图 160 ms；小地图 SURF 校准保持 250 ms 防丢锁触发 OCR
            std::chrono::milliseconds{200},
            std::chrono::milliseconds{1000}
        };
    case PerformanceProfile::Custom: {
        const auto maxPeriod = PeriodFromFps(settings.captureMaxFps);
        const int mapMs = ClampMs(settings.mapUpdateCycleMs, 16, 1000);
        const int minMapMs = ClampMs(settings.minMapUpdateCycleMs, 16, 1000);
        const int ocrMs = ClampMs(settings.ocrUpdateCycleMs, 50, 1000);
        const auto ocrPeriod = std::chrono::microseconds(static_cast<std::int64_t>(ocrMs) * 1000);
        // 大地图与小地图视觉均按采集帧率上限做像素粘合；OCR 模式自动不快于 OCR 间隔。
        const auto readoutPeriod = ocrPeriod > maxPeriod ? ocrPeriod : maxPeriod;
        const auto idlePeriod = std::chrono::microseconds{125000} > maxPeriod
            ? std::chrono::microseconds{125000} : maxPeriod;
        const int probeMs = ClampMs(ocrMs * 4, 300, 1000);
        return {
            profile,
            maxPeriod,
            maxPeriod,
            readoutPeriod,
            idlePeriod,
            mapMs,
            minMapMs,
            std::chrono::milliseconds{ocrMs},
            std::chrono::milliseconds{probeMs}
        };
    }
    case PerformanceProfile::Balanced:
    default:
        return {
            PerformanceProfile::Balanced,
            std::chrono::microseconds{50000},   // 大地图 20 fps（拖拽跟手）
            std::chrono::microseconds{50000},   // 小地图视觉 20 fps（高频像素粘合）
            std::chrono::microseconds{150000},  // OCR 模式对齐 150 ms（~6.7 fps）
            std::chrono::microseconds{125000},  // 空闲巡检 8 fps
            100, 250,                           // 大地图 100 ms；小地图 SURF 绝对校准 250 ms
            std::chrono::milliseconds{150},
            std::chrono::milliseconds{600}
        };
    }
}

// 获取当前场景的基础采集周期（不含慢帧退避），可直接下发给 WGC MinUpdateInterval。
inline std::chrono::microseconds ResolveBaseCapturePeriod(
    const EffectivePacing& pacing, CaptureScenario scenario) {
    switch (scenario) {
    case CaptureScenario::BigMap: return pacing.bigMapCapturePeriod;
    case CaptureScenario::MinimapVisual: return pacing.minimapVisualCapturePeriod;
    case CaptureScenario::MinimapReadout: return pacing.minimapReadoutCapturePeriod;
    default: return pacing.idleCapturePeriod;
    }
}

// 获取采集线程实际等待周期（含慢帧退避）。
inline std::chrono::microseconds ResolveScenarioCapturePeriod(
    const EffectivePacing& pacing, CaptureScenario scenario, bool slowCapture) {
    return ResolveBaseCapturePeriod(pacing, scenario) +
        (slowCapture ? kSlowCaptureBackoff : std::chrono::microseconds::zero());
}

// 采集帧的有效期需随低频档位放宽，避免 5~7 fps 时相邻两帧之间被误判为过期。
inline std::chrono::milliseconds CaptureFrameMaxAge(std::chrono::microseconds baseCapturePeriod) {
    const auto tripleMs = (baseCapturePeriod.count() * 3 + 999) / 1000;
    const auto clamped = tripleMs < 250 ? 250 : (tripleMs > 1000 ? 1000 : tripleMs);
    return std::chrono::milliseconds(clamped);
}

// Startup reads one frame to prove the capture works, and that frame is already stale by the time the
// capture loop runs - it is not a frame the loop observed. The loop therefore takes the first sequence
// it sees as its baseline and needs a newer one before it publishes, instead of re-publishing startup
// pixels as if they were current.
struct CaptureSequenceFilter {
    bool baselineKnown = false;
    std::uint64_t baseline = 0;
    std::uint64_t lastPublished = 0;

    // Returns true when this sequence is a frame the loop should publish, and records it.
    bool Accept(std::uint64_t sequence) {
        if (!baselineKnown) { baselineKnown = true; baseline = sequence; }
        if (sequence == baseline || sequence == lastPublished) return false;
        lastPublished = sequence;
        return true;
    }
};

// The capture loop publishes a frame id that every consumer uses as "have I seen this frame". Two kinds
// of backend have to end up in that one id space:
//
//  - A backend that numbers its frames (Windows Graphics Capture) hands the loop the same number
//    App::Init already consumed, so the filter skips exactly that one.
//  - A backend that numbers nothing (PrintWindow, this path's BitBlt fallback) reports no sequence at
//    all, and every successful call is a frame the loop has never published. The loop numbers those
//    itself, and it must hand out a *new* id each time, because a consumer that sees an unchanged id
//    concludes there is no new frame and never looks at the pixels.
//
// Those two cases cannot share the filter: the loop's synthetic number is derived from the last id it
// published, so when the filter swallows the first frame that number never advances - the same id is
// offered again forever and the tool publishes nothing for the whole session. Keep the decision here so
// the test can cover the numbering itself instead of only the filter in isolation.
struct CaptureFrameSource {
    CaptureSequenceFilter filter;
    std::uint64_t lastPublished = 0;

    // sourceSequence is the backend's frame number, or 0 when the backend reports none. Returns the
    // frame id to publish, or 0 when this frame must not be published.
    std::uint64_t Publish(std::uint64_t sourceSequence) {
        if (sourceSequence == 0) return ++lastPublished;
        if (!filter.Accept(sourceSequence)) return 0;
        lastPublished = sourceSequence;
        return sourceSequence;
    }
};

// The overlay window covers the whole game screen, so every present makes the desktop compositor
// blend that whole screen again - including the game's own frames, which a visible topmost layered
// window keeps out of its direct flip path. A frame whose content did not change therefore costs the
// game GPU time for nothing, and is skipped: the compositor keeps showing the last surface.
inline bool ShouldPresentFrame(std::uint64_t contentHash, std::uint64_t lastPresentedHash,
    bool hasPresented, bool windowVisible) {
    if (!hasPresented || !windowVisible) return true;
    return contentHash != lastPresentedHash;
}

// Nothing drawn for this many overlay frames (one second at the overlay rate) means the tool has
// nothing to show at all: hiding the window lets the compositor ignore it completely until the next
// marker appears.
inline constexpr int kFramesBeforeHidingIdleOverlay = 30;

inline bool ShouldHideIdleOverlay(int consecutiveEmptyFrames) {
    return consecutiveEmptyFrames >= kFramesBeforeHidingIdleOverlay;
}

// Returning focus from the route tools can briefly leave Windows with no foreground HWND. The
// capture thread then publishes a non-displayable map frame before the game has a fresh one.
// Keep the already presented map surface for only that handoff, without moving or repainting it.
inline constexpr std::chrono::milliseconds kFocusHandoffHoldLimit{350};
enum class FocusHandoffForeground { Tools, Game, None, Other };

struct FocusHandoffHold {
    Clock::time_point lastReadyToolsFrame{};

    bool ShouldHold(bool toolsRegistered, FocusHandoffForeground foreground, bool mapReady,
        bool presentedMap, Clock::time_point now) {
        if (foreground == FocusHandoffForeground::Other) { lastReadyToolsFrame = {}; return false; }
        if (foreground == FocusHandoffForeground::Tools) {
            if (toolsRegistered && mapReady && presentedMap) lastReadyToolsFrame = now;
            return false;
        }
        if (foreground == FocusHandoffForeground::Game && mapReady) {
            lastReadyToolsFrame = {}; return false;
        }
        if (!presentedMap || lastReadyToolsFrame == Clock::time_point{} ||
            now < lastReadyToolsFrame || now - lastReadyToolsFrame >= kFocusHandoffHoldLimit) {
            lastReadyToolsFrame = {}; return false;
        }
        return true;
    }
};

// Diagnostic only. A visible topmost layered window costs the game two different things: the desktop
// compositor has to keep it in the composition, and every present makes the compositor blend the
// whole screen again. Holding the presents lets a frame-rate comparison say which of the two a
// measurement is actually paying for, because a held overlay keeps its window in the composition
// while skipping the render, the present and everything leading up to them.
//
// Markers are never held: their screen position changes with the map, so a held overlay would show
// them in the wrong place. Only a frame whose content is the status bar alone is a candidate.
inline constexpr int kHeldFrameInterval = 90; // about three seconds at the overlay rate

struct HoldPresentPolicy {
    bool holding = false;
    int heldFrames = 0;
    /// needsMarkers: whether the frame about to be rendered has markers to draw at a tracked position.
    /// Those are never held - their screen position moves with the map, so a held overlay would show
    /// them in the wrong place, and a frame that stopped needing them has to reach the screen to clear
    /// them. Only a frame with nothing but the status bar is a candidate.
    bool ShouldHold(bool needsMarkers) {
        if (holding) {
            if (++heldFrames >= kHeldFrameInterval) { holding = false; heldFrames = 0; }
            return holding;
        }
        if (needsMarkers) return false;
        holding = true;
        heldFrames = 0; // Counted when the next frame asks to hold, so the interval is exact.
        return true;
    }
};
}
