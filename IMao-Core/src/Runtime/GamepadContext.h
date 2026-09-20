#pragma once
#include "../Domain/MapData.h"
#include "NearbySelection.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// Capture, localization and IPC run on different threads. Only this model owns
// the gamepad context; consumers never dereference the mutable App instance.
class GamepadContextSnapshot {
public:
    using Clock = std::chrono::steady_clock;
    using Candidate = NearbySelection::Candidate;
    struct View {
        bool running = false, observable = false, bigMap = false, gameplay = false, nearbyAvailable = false;
        std::uint64_t session = 0, generation = 1, gameHwnd = 0;
        std::uint32_t gameProcessId = 0;
        std::string profileId, sceneName;
        std::vector<Candidate> candidates;
    };
    static GamepadContextSnapshot& Shared() { static GamepadContextSnapshot value; return value; }

    void Begin(std::uint64_t session, std::uint64_t hwnd, std::uint32_t processId) {
        std::scoped_lock lock(mutex_);
        const auto next = view_.generation + 1;
        view_ = {}; view_.generation = next; view_.running = true;
        view_.session = session; view_.gameHwnd = hwnd; view_.gameProcessId = processId;
        recent_.reset(); mapSession_ = false; uiAt_ = {}; openedAt_ = {};
    }
    void End(std::uint64_t session) {
        std::scoped_lock lock(mutex_);
        if (view_.session != session) return;
        InvalidateLocked(true); view_.running = false;
    }
    void Invalidate(std::uint64_t session) {
        std::scoped_lock lock(mutex_);
        if (view_.session == session) InvalidateLocked(true);
    }

    // This uses current visual evidence, not the map-state debouncer alone.
    // Missing evidence revokes the context on its first frame. The last player
    // sample may bridge the short opening animation, but expires in one second.
    void ObserveUi(std::uint64_t session, const std::string& profile,
        bool stableMap, bool mapEvidence, bool minimapEvidence, bool observable,
        Clock::time_point capturedAt, std::chrono::milliseconds maximumAge,
        Clock::time_point now = Clock::now()) {
        std::scoped_lock lock(mutex_);
        if (!ActiveLocked(session)) return;
        ProfileLocked(profile);
        // A resumed capture is a new context even if no IPC reader happened to
        // observe the preceding stall before this callback arrived.
        if (view_.observable && (now < uiAt_ || now - uiAt_ >= uiMaximumAge_)) InvalidateLocked(true);
        if (now < capturedAt || now - capturedAt >= maximumAge) {
            InvalidateLocked(true); return;
        }
        uiAt_ = capturedAt; uiMaximumAge_ = maximumAge;
        if (!observable || (!mapEvidence && !minimapEvidence)) {
            InvalidateLocked(false); return;
        }
        view_.observable = true;
        if (minimapEvidence) {
            if (mapSession_) InvalidateLocked(true);
            view_.observable = true; view_.bigMap = false; view_.gameplay = true;
            return;
        }
        view_.gameplay = false;
        if (!mapSession_) {
            ++view_.generation; mapSession_ = true; openedAt_ = capturedAt;
            view_.nearbyAvailable = recent_ && recent_->profile == profile &&
                now >= recent_->locatedAt && now - recent_->locatedAt <= std::chrono::seconds(1) &&
                now < recent_->freshUntil;
            view_.candidates.clear();
            if (view_.nearbyAvailable) {
                view_.sceneName = recent_->scene;
                view_.candidates = recent_->candidates;
            }
            recent_.reset(); // Never re-freeze this location into a later map session.
        }
        view_.bigMap = stableMap;
    }

    void ObserveMinimap(std::uint64_t session, const std::string& profile, const ItemMarkerFrame& frame,
        const Coordinate& playerROC, Clock::time_point locatedAt, Clock::time_point freshUntil,
        Clock::time_point now = Clock::now(), double pixelsPerMapUnit = 0) {
        std::scoped_lock lock(mutex_);
        if (!ActiveLocked(session) || mapSession_ || frame.profileId != profile || frame.sceneName.empty() ||
            !std::isfinite(playerROC.x) || !std::isfinite(playerROC.y) || now < locatedAt ||
            now - locatedAt > std::chrono::seconds(1) || now >= freshUntil) return;
        ProfileLocked(profile);
        if (!view_.sceneName.empty() && view_.sceneName != frame.sceneName) ++view_.generation;
        view_.sceneName = frame.sceneName;
        Location sample{profile, frame.sceneName, locatedAt, freshUntil,
            NearbySelection::Collect(frame, playerROC, pixelsPerMapUnit), frame.filterRevision, frame.markerRadius};
        recent_ = std::move(sample);
    }

    void ObserveMapScene(std::uint64_t session, const std::string& profile, const std::string& scene,
        Clock::time_point capturedAt) {
        std::scoped_lock lock(mutex_);
        if (!ActiveLocked(session) || !mapSession_ || scene.empty() || capturedAt < openedAt_) return;
        ProfileLocked(profile);
        if (!view_.sceneName.empty() && view_.sceneName != scene) {
            ++view_.generation; view_.nearbyAvailable = false; view_.candidates.clear(); recent_.reset();
        }
        view_.sceneName = scene;
    }

    View Read(const std::string& profile, Clock::time_point now = Clock::now()) {
        std::scoped_lock lock(mutex_);
        ProfileLocked(profile);
        if (view_.observable && (now < uiAt_ || now - uiAt_ >= uiMaximumAge_)) InvalidateLocked(true);
        return view_;
    }
    NearbySelection::Observation ReadNearby(const std::string& profile, Clock::time_point now = Clock::now()) {
        std::scoped_lock lock(mutex_);
        ProfileLocked(profile);
        if (view_.observable && (now < uiAt_ || now - uiAt_ >= uiMaximumAge_)) InvalidateLocked(true);
        NearbySelection::Observation result;
        result.session = view_.session; result.gameHwnd = view_.gameHwnd; result.gameProcessId = view_.gameProcessId;
        result.profileId = view_.profileId; result.sceneName = view_.sceneName;
        if (!recent_) return result;
        result.filterRevision = recent_->filterRevision;
        result.markerRadius = recent_->markerRadius;
        result.locatedAt = recent_->locatedAt; result.freshUntil = recent_->freshUntil;
        result.available = view_.running && view_.observable && view_.gameplay && !mapSession_ &&
            recent_->profile == profile && recent_->scene == view_.sceneName && now >= recent_->locatedAt &&
            now - recent_->locatedAt <= std::chrono::seconds(1) && now < recent_->freshUntil;
        if (result.available) result.candidates = recent_->candidates;
        return result;
    }
private:
    struct Location {
        std::string profile, scene;
        Clock::time_point locatedAt, freshUntil;
        std::vector<Candidate> candidates;
        std::uint64_t filterRevision = 0;
        double markerRadius = 0;
    };
    bool ActiveLocked(std::uint64_t session) const { return view_.running && view_.session == session; }
    void ProfileLocked(const std::string& profile) {
        if (view_.profileId == profile) return;
        InvalidateLocked(true); ++view_.generation; view_.profileId = profile; view_.sceneName.clear();
    }
    void InvalidateLocked(bool clearRecent) {
        if (view_.observable || mapSession_ || view_.nearbyAvailable || (clearRecent && recent_)) ++view_.generation;
        view_.observable = view_.bigMap = view_.gameplay = view_.nearbyAvailable = mapSession_ = false;
        view_.candidates.clear();
        if (clearRecent) recent_.reset();
    }
    std::mutex mutex_;
    View view_;
    std::optional<Location> recent_;
    bool mapSession_ = false;
    Clock::time_point uiAt_{}, openedAt_{};
    std::chrono::milliseconds uiMaximumAge_{500};
};
