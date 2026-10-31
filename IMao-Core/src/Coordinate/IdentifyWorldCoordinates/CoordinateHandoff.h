#pragma once
#include <chrono>
#include <cmath>
#include <cstdint>
#include <algorithm>

// Source health is independent of the display lock: OCR must never heal SURF.
class CoordinateHandoff {
public:
    using Clock = std::chrono::steady_clock;
    enum class VisualSource { Unspecified, Local, Global };
    // A committed position, validated by the caller for the current UI generation.
    // Coordinates use the same world units as ConfirmReadout, never image-map pixels.
    struct ReadoutPrior {
        bool valid = false;
        int scene = 0;
        std::uint64_t frame = 0;
        Clock::time_point capturedAt{};
        double x = 0, y = 0;
    };
    explicit CoordinateHandoff(std::chrono::milliseconds stableDuration = std::chrono::milliseconds(0))
        : stableDuration_(stableDuration) {}
    void Reset() { *this = CoordinateHandoff(stableDuration_); }
    bool ReadoutActive() const { return readoutActive_; }
    bool NeedsReadout(Clock::time_point now) const {
        return readoutActive_ || failing_ || (!Healthy(localHealth_, now) && !Healthy(globalHealth_, now));
    }
    std::chrono::milliseconds Cadence(Clock::time_point now) const {
        if (readoutActive_) return std::chrono::milliseconds(150);
        return NeedsReadout(now) ? std::chrono::milliseconds(250) : std::chrono::milliseconds(1000);
    }
    bool ObserveFailure(VisualSource source = VisualSource::Unspecified, Clock::time_point capturedAt = {}) {
        auto& health = source == VisualSource::Global ? globalHealth_ : localHealth_;
        if (source != VisualSource::Unspecified && capturedAt != Clock::time_point{}) {
            if (capturedAt <= health.observed) return false;
            health.observed = capturedAt;
        }
        if (source == VisualSource::Unspecified) { failing_ = true; localHealth_.failed = globalHealth_.failed = true; }
        else (source == VisualSource::Global ? globalHealth_ : localHealth_).failed = true;
        if (source == VisualSource::Local) localRecovery_ = {};
        if (source == VisualSource::Global) globalRecovery_ = {};
        return true;
    }
    void ObserveReadout(Clock::time_point now) {
        if (!readoutActive_) localRecovery_ = globalRecovery_ = {};
        readoutActive_ = true; lastReadout_ = now;
    }
    void RejectVisual(std::uint64_t frame, Clock::time_point capturedAt, VisualSource source) {
        if (capturedAt > lastVisual_ && frame != lastFrame_) ObserveFailure(source, capturedAt);
    }
    bool FreshReadout(std::uint64_t frame, Clock::time_point capturedAt, Clock::time_point now) const {
        // The caller validates session/generation and ordering against the committed
        // visual position. Slow inference alone does not invalidate this sample.
        return frame > readFrame_ && capturedAt != Clock::time_point{} &&
            capturedAt > readCapture_ && capturedAt <= now;
    }
    void BreakReadoutStreak() { readCount_ = 0; }
    bool ConfirmReadout(std::uint64_t frame, Clock::time_point capturedAt, int scene, double x, double y,
        const ReadoutPrior* prior = nullptr) {
        usedReadoutPrior_ = false;
        if (!std::isfinite(x) || !std::isfinite(y)) return false;
        if(frame <= readFrame_ || capturedAt <= readCapture_)return false;
        const double elapsed=std::chrono::duration<double>(capturedAt-readCapture_).count();
        const bool agrees=readCount_>0 && scene==readScene_ && elapsed<=1.5 &&
            std::hypot(x-readX_,y-readY_)<=(std::max)(30.,100.*elapsed);
        // A blank read is not evidence that a recently committed position was
        // wrong. It can corroborate this new, separately gated observation.
        if (prior && prior->valid && prior->scene == scene && prior->frame != 0 && prior->frame < frame &&
            prior->capturedAt != Clock::time_point{} && prior->capturedAt < capturedAt &&
            capturedAt - prior->capturedAt <= std::chrono::milliseconds(1500) &&
            std::isfinite(prior->x) && std::isfinite(prior->y)) {
            const auto seconds = std::chrono::duration<double>(capturedAt - prior->capturedAt).count();
            usedReadoutPrior_ = std::hypot(x-prior->x,y-prior->y) <= (std::max)(30.,100.*seconds);
        }
        readCount_=agrees?readCount_+1:1;
        if (usedReadoutPrior_) readCount_=(std::max)(2,readCount_);
        readFrame_=frame;readCapture_=capturedAt;
        readX_=x;readY_=y;readScene_=scene;
        return readCount_>=2;
    }
    bool UsedReadoutPrior() const { return usedReadoutPrior_; }
    bool ObserveVisual(std::uint64_t frame, Clock::time_point capturedAt, double x, double y, int scene = 1,
        VisualSource source = VisualSource::Local, bool mayPublish = true) {
        auto& health = source == VisualSource::Global ? globalHealth_ : localHealth_;
        if (!std::isfinite(x) || !std::isfinite(y) || capturedAt <= lastVisual_ || capturedAt <= health.observed) return false;
        if (frame == lastFrame_) return false;
        lastVisual_ = capturedAt; lastFrame_ = frame;
        health = {capturedAt, false};
        if (readoutActive_) {
            auto& recovery = source == VisualSource::Global ? globalRecovery_ : localRecovery_;
            const bool consistent = recovery.count > 0 && scene == recovery.scene &&
                capturedAt - recovery.last <= std::chrono::milliseconds(750) &&
                std::hypot(x - recovery.x, y - recovery.y) <= 120.0 * 1.205;
            if (!consistent) { recovery = {}; recovery.start = capturedAt; }
            ++recovery.count; recovery.last = capturedAt; recovery.scene = scene; recovery.x = x; recovery.y = y;
            if (recovery.count < 2 || capturedAt - recovery.start < stableDuration_) return false;
        }
        if (!mayPublish) return false; // Health evidence cannot roll back a newer position.
        readoutActive_ = false; failing_ = false; localRecovery_ = globalRecovery_ = {};
        return true;
    }
private:
    struct SourceHealth { Clock::time_point observed{}; bool failed = true; };
    static bool Healthy(const SourceHealth& health, Clock::time_point now) {
        return !health.failed && health.observed != Clock::time_point{} && now >= health.observed &&
            now - health.observed <= std::chrono::milliseconds(500);
    }
    SourceHealth localHealth_, globalHealth_;
    bool readoutActive_ = false, failing_ = false;
    struct Recovery {
        int count = 0, scene = 0;
        double x = 0, y = 0;
        Clock::time_point start{}, last{};
    };
    Recovery localRecovery_, globalRecovery_;
    std::chrono::milliseconds stableDuration_;
    std::uint64_t lastFrame_ = 0;
    Clock::time_point lastVisual_{}, lastReadout_{};
    Clock::time_point readCapture_{};
    std::uint64_t readFrame_=0;
    int readScene_=0,readCount_=0;
    double readX_=0,readY_=0;
    bool usedReadoutPrior_ = false;
};
