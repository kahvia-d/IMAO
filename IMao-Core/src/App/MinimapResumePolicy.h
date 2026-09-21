#pragma once

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

struct MinimapPosition {
    int sceneId = 0;
    double x = 0.0;
    double y = 0.0;

    bool Valid() const { return sceneId > 0 && std::isfinite(x) && std::isfinite(y); }
};

// A match, including a Strong match, is only one observation during recovery.
// Confirmation must come from another captured image in the same UI session.
class MinimapVisualConfirmation {
public:
    using Clock = std::chrono::steady_clock;

    void Reset() { pending_.reset(); }
    bool Pending() const { return pending_.has_value(); }

    bool Observe(std::uint64_t generation, std::uint64_t frameId,
        const MinimapPosition& position, Clock::time_point now = Clock::now()) {
        if (generation == 0 || frameId == 0 || !position.Valid()) {
            Reset();
            return false;
        }
        if (pending_.has_value() && pending_->generation == generation &&
            frameId <= pending_->frameId) return false;
        const bool confirmed = pending_.has_value() && pending_->generation == generation &&
            now >= pending_->observedAt && now - pending_->observedAt <= std::chrono::seconds(2) &&
            pending_->position.sceneId == position.sceneId &&
            std::hypot(pending_->position.x - position.x, pending_->position.y - position.y) <= 12.0;
        pending_ = Observation{ generation, frameId, position, now };
        return confirmed;
    }

private:
    struct Observation {
        std::uint64_t generation;
        std::uint64_t frameId;
        MinimapPosition position;
        Clock::time_point observedAt;
    };
    std::optional<Observation> pending_;
};

enum class MinimapResumeSource { TrustedMinimap, MapViewport };

struct MinimapResumeHint {
    MinimapResumeSource source = MinimapResumeSource::TrustedMinimap;
    MinimapPosition position;
    MinimapVisualConfirmation::Clock::time_point observedAt{};
    std::uint64_t viewportGeneration = 0;
    std::uint64_t viewportRevision = 0;
};

// These are bounded retrieval hints, never player-coordinate evidence. Keep
// the original observation timestamp; opening/closing the map cannot renew it.
class MinimapResumePolicy {
public:
    using Clock = MinimapVisualConfirmation::Clock;
    struct Attempt {
        MinimapResumeHint hint;
        std::uint64_t generation;
        std::uint64_t frameId;
        std::size_t index;
        int ordinal;
    };

    static const char* SourceName(MinimapResumeSource source) {
        return source == MinimapResumeSource::TrustedMinimap ? "trusted-minimap" : "recent-map-viewport";
    }

    static bool Fresh(const MinimapResumeHint& hint, Clock::time_point now) {
        const auto maximumAge = hint.source == MinimapResumeSource::MapViewport
            ? std::chrono::seconds(30) : std::chrono::seconds(300);
        return hint.position.Valid() && now >= hint.observedAt && now - hint.observedAt <= maximumAge;
    }

    void Reset() {
        generation_ = 0;
        count_ = index_ = 0;
        lastAttemptFrame_ = 0;
        confirmation_.Reset();
    }

    void Begin(std::uint64_t generation, const std::optional<MinimapResumeHint>& trusted,
        const std::optional<MinimapResumeHint>& viewport, std::uint64_t viewportGeneration,
        std::uint64_t viewportRevision, Clock::time_point now = Clock::now()) {
        Reset();
        generation_ = generation;
        if (trusted.has_value() && trusted->source == MinimapResumeSource::TrustedMinimap && Fresh(*trusted, now))
            entries_[count_++] = Entry{ *trusted };
        // The generation identifies the map session, and that is what must match.  The revision
        // is the *bridge frame counter*, not a pan/zoom generation: requiring equality dropped
        // the hint one frame after it was recorded (2026-09-21 11:03 field log: the hint carried
        // revision 11 while the observed revision had reached 13, so closing the map produced
        // hints=0 and the minimap had nothing to revalidate against).  A hint cannot be newer
        // than what has been observed, and its age is capped by Fresh() - it is a bounded
        // retrieval prior whose candidate must still be confirmed by a second captured frame.
        if (viewport.has_value() && viewport->source == MinimapResumeSource::MapViewport && Fresh(*viewport, now) &&
            viewport->viewportGeneration == viewportGeneration &&
            viewport->viewportRevision <= viewportRevision)
            entries_[count_++] = Entry{ *viewport };
    }

    std::uint64_t Generation() const { return generation_; }
    std::size_t Size() const { return count_; }
    bool AwaitingConfirmation() const { return confirmation_.Pending(); }

    std::optional<Attempt> NextAttempt(std::uint64_t generation, std::uint64_t frameId,
        Clock::time_point now = Clock::now()) {
        if (generation != generation_ || frameId == 0 || frameId <= lastAttemptFrame_) return std::nullopt;
        while (index_ < count_) {
            auto& entry = entries_[index_];
            if (!Fresh(entry.hint, now) || entry.failures >= 2 || entry.attempts >= 4) {
                ++index_;
                confirmation_.Reset();
                continue;
            }
            lastAttemptFrame_ = frameId;
            return Attempt{ entry.hint, generation, frameId, index_, ++entry.attempts };
        }
        return std::nullopt;
    }

    bool Observe(const Attempt& attempt, bool geometricSupport, const MinimapPosition& candidate,
        Clock::time_point now = Clock::now()) {
        if (attempt.generation != generation_ || attempt.index != index_ || index_ >= count_ ||
            attempt.frameId != lastAttemptFrame_ || attempt.ordinal != entries_[index_].attempts ||
            attempt.ordinal == entries_[index_].lastObservedAttempt) return false;
        auto& entry = entries_[index_];
        entry.lastObservedAttempt = attempt.ordinal;
        if (!geometricSupport || !candidate.Valid() || candidate.sceneId != entry.hint.position.sceneId ||
            !Fresh(entry.hint, now)) {
            ++entry.failures;
            confirmation_.Reset();
            return false;
        }
        return confirmation_.Observe(generation_, attempt.frameId, candidate, now);
    }

private:
    struct Entry {
        MinimapResumeHint hint;
        int attempts = 0;
        int failures = 0;
        int lastObservedAttempt = 0;
    };
    std::array<Entry, 2> entries_{};
    std::size_t count_ = 0;
    std::size_t index_ = 0;
    std::uint64_t generation_ = 0;
    std::uint64_t lastAttemptFrame_ = 0;
    MinimapVisualConfirmation confirmation_;
};
