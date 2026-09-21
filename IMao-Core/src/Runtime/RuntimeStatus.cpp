#include "RuntimeStatus.h"

#include <algorithm>
#include <utility>

RuntimeStatusSnapshot RuntimeStatus::status{};
std::mutex RuntimeStatus::mutex;

RuntimeStatusSnapshot RuntimeStatus::Snapshot() {
    std::scoped_lock lock(mutex);
    return status;
}

void RuntimeStatus::TouchLocked() {
    ++status.sequence;
}

void RuntimeStatus::SetCoreState(std::string state, std::string message) {
    std::scoped_lock lock(mutex);
    status.coreState = std::move(state);
    if (!message.empty()) status.message = std::move(message);
    TouchLocked();
}

void RuntimeStatus::SetGameState(std::string state, bool focused) {
    std::scoped_lock lock(mutex);
    status.gameState = std::move(state);
    status.gameFocused = focused;
    TouchLocked();
}

void RuntimeStatus::SetLocalization(std::string localization, std::string quality, std::string message) {
    std::scoped_lock lock(mutex);
    status.localization = std::move(localization);
    status.quality = std::move(quality);
    if (status.localization != "stale") status.lastGoodAgeMilliseconds = 0;
    if (!message.empty()) status.message = std::move(message);
    TouchLocked();
}

void RuntimeStatus::SetMarkerCounts(int minimapMarkers, int mapMarkers) {
    std::scoped_lock lock(mutex);
    status.minimapMarkers = std::max(0, minimapMarkers);
    status.mapMarkers = std::max(0, mapMarkers);
    TouchLocked();
}

void RuntimeStatus::SetMinimapMarkerCount(int minimapMarkers) {
    std::scoped_lock lock(mutex);
    status.minimapMarkers = std::max(0, minimapMarkers);
    TouchLocked();
}

void RuntimeStatus::SetMapMarkerCount(int mapMarkers) {
    std::scoped_lock lock(mutex);
    status.mapMarkers = std::max(0, mapMarkers);
    TouchLocked();
}

void RuntimeStatus::SetFrameMilliseconds(int milliseconds) {
    std::scoped_lock lock(mutex);
    status.frameMilliseconds = std::max(0, milliseconds);
    TouchLocked();
}

void RuntimeStatus::SetLastGoodAgeMilliseconds(int milliseconds) {
    std::scoped_lock lock(mutex);
    status.lastGoodAgeMilliseconds = std::max(0, milliseconds);
    TouchLocked();
}

void RuntimeStatus::SetMinimapFeatureStats(int rawKeypoints, int retainedKeypoints, int dynamicMaskPercent) {
    std::scoped_lock lock(mutex);
    status.minimapRawKeypoints = std::max(0, rawKeypoints);
    status.minimapRetainedKeypoints = std::max(0, retainedKeypoints);
    status.minimapDynamicMaskPercent = std::clamp(dynamicMaskPercent, 0, 100);
    TouchLocked();
}

void RuntimeStatus::SetStatusBarEnabled(bool enabled) {
    std::scoped_lock lock(mutex);
    status.statusBarEnabled = enabled;
    TouchLocked();
}

void RuntimeStatus::SetMessage(std::string message) {
    std::scoped_lock lock(mutex);
    status.message = std::move(message);
    TouchLocked();
}

void RuntimeStatus::SetLocalizationHint(std::string hint) {
    std::scoped_lock lock(mutex);
    if (status.hint == hint) return;
    status.hint = std::move(hint);
    TouchLocked();
}
