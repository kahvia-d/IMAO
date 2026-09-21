#pragma once

#include <cstdint>
#include <mutex>
#include <string>

struct RuntimeStatusSnapshot {
    std::uint64_t sequence = 0;
    std::string coreState = "starting";
    std::string gameState = "unknown";
    std::string localization = "waiting";
    std::string quality = "";
    std::string message = "正在启动核心";
    // An action the player can take to unblock localization.  The in-game status bar has room
    // for one line of it, and "the markers are frozen" is not something a message channel that
    // scrolls with notifications can explain.
    std::string hint = "";
    int minimapMarkers = 0;
    int mapMarkers = 0;
    int frameMilliseconds = 0;
    int lastGoodAgeMilliseconds = 0;
    int minimapRawKeypoints = 0;
    int minimapRetainedKeypoints = 0;
    int minimapDynamicMaskPercent = 0;
    bool gameFocused = false;
    bool statusBarEnabled = true;
};

// One small, synchronized status model powers the IPC client and the in-game
// status bar.  It deliberately contains only already-computed values: reading
// it must never trigger capture, OCR, or feature matching.
class RuntimeStatus {
public:
    static RuntimeStatusSnapshot Snapshot();
    static void SetCoreState(std::string state, std::string message = {});
    static void SetGameState(std::string state, bool focused);
    static void SetLocalization(std::string localization, std::string quality = {}, std::string message = {});
    static void SetMarkerCounts(int minimapMarkers, int mapMarkers);
    static void SetMinimapMarkerCount(int minimapMarkers);
    static void SetMapMarkerCount(int mapMarkers);
    static void SetFrameMilliseconds(int milliseconds);
    static void SetLastGoodAgeMilliseconds(int milliseconds);
    static void SetMinimapFeatureStats(int rawKeypoints, int retainedKeypoints, int dynamicMaskPercent);
    static void SetStatusBarEnabled(bool enabled);
    static void SetMessage(std::string message);
    static void SetLocalizationHint(std::string hint);

private:
    static void TouchLocked();
    static RuntimeStatusSnapshot status;
    static std::mutex mutex;
};
