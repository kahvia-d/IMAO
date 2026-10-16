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
    explicit CoordinateHandoff(std::chrono::milliseconds stableDuration = std::chrono::milliseconds(0))
        : stableDuration_(stableDuration) {}
    void Reset() { *this = CoordinateHandoff(stableDuration_); }
    bool ReadoutActive() const { return readoutActive_; }
    bool NeedsReadout(Clock::time_point now) const {
        return readoutActive_ || failing_ || lastVisual_ == Clock::time_point{} ||
            now - lastVisual_ > std::chrono::milliseconds(500);
    }
    std::chrono::milliseconds Cadence(Clock::time_point now) const {
        return NeedsReadout(now) ? std::chrono::milliseconds(250) : std::chrono::milliseconds(1000);
    }
    void ObserveFailure(VisualSource source = VisualSource::Unspecified) {
        failing_ = true;
        if (source == VisualSource::Local) localRecovery_ = {};
        if (source == VisualSource::Global) globalRecovery_ = {};
    }
    void ObserveReadout(Clock::time_point now) {
        if (!readoutActive_) localRecovery_ = globalRecovery_ = {};
        readoutActive_ = true; lastReadout_ = now;
    }
    void RejectVisual(std::uint64_t frame, Clock::time_point capturedAt, VisualSource source) {
        if (capturedAt > lastVisual_ && frame != lastFrame_) ObserveFailure(source);
    }
    bool FreshReadout(std::uint64_t frame, Clock::time_point capturedAt, Clock::time_point now) const {
        return frame != readFrame_ && capturedAt != Clock::time_point{} && capturedAt <= now &&
            now - capturedAt <= std::chrono::milliseconds(750);
    }
    void BreakReadoutStreak() { readCount_ = 0; }
    bool ConfirmReadout(std::uint64_t frame, Clock::time_point capturedAt, int scene, double x, double y) {
        if(frame == readFrame_ || capturedAt <= readCapture_)return false;
        const double elapsed=std::chrono::duration<double>(capturedAt-readCapture_).count();
        const bool agrees=readCount_>0 && scene==readScene_ && elapsed<=1.5 &&
            std::hypot(x-readX_,y-readY_)<=(std::max)(30.,100.*elapsed);
        readCount_=agrees?readCount_+1:1;readFrame_=frame;readCapture_=capturedAt;
        readX_=x;readY_=y;readScene_=scene;
        return readCount_>=2;
    }
    bool ObserveVisual(std::uint64_t frame, Clock::time_point capturedAt, double x, double y, int scene = 1,
        VisualSource source = VisualSource::Local) {
        if (!std::isfinite(x) || !std::isfinite(y) || capturedAt <= lastVisual_) return false;
        if (frame == lastFrame_) return false;
        lastVisual_ = capturedAt; lastFrame_ = frame;
        if (readoutActive_) {
            auto& recovery = source == VisualSource::Global ? globalRecovery_ : localRecovery_;
            const bool consistent = recovery.count > 0 && scene == recovery.scene &&
                capturedAt - recovery.last <= std::chrono::milliseconds(750) &&
                std::hypot(x - recovery.x, y - recovery.y) <= 120.0 * 1.205;
            if (!consistent) { recovery = {}; recovery.start = capturedAt; }
            ++recovery.count; recovery.last = capturedAt; recovery.scene = scene; recovery.x = x; recovery.y = y;
            if (recovery.count < 2 || capturedAt - recovery.start < stableDuration_) return false;
        }
        readoutActive_ = false; failing_ = false; localRecovery_ = globalRecovery_ = {};
        return true;
    }
private:
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
};
