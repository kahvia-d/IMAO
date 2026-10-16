#pragma once
#include <chrono>
#include <cmath>
#include <cstdint>
#include <algorithm>

// Source health is independent of the display lock: OCR must never heal SURF.
class CoordinateHandoff {
public:
    using Clock = std::chrono::steady_clock;
    void Reset() { *this = {}; }
    bool ReadoutActive() const { return readoutActive_; }
    bool NeedsReadout(Clock::time_point now) const {
        return readoutActive_ || failing_ || lastVisual_ == Clock::time_point{} ||
            now - lastVisual_ > std::chrono::milliseconds(500);
    }
    std::chrono::milliseconds Cadence(Clock::time_point now) const {
        return NeedsReadout(now) ? std::chrono::milliseconds(250) : std::chrono::milliseconds(1000);
    }
    void ObserveFailure() { failing_ = true; if (!readoutActive_) recoveryCount_ = 0; }
    void ObserveReadout(Clock::time_point now) { readoutActive_ = true; lastReadout_ = now; }
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
    bool ObserveVisual(std::uint64_t frame, Clock::time_point capturedAt, double x, double y, int scene = 1) {
        if (!std::isfinite(x) || !std::isfinite(y) || capturedAt <= lastVisual_) return false;
        if (frame == lastFrame_) return false;
        const double elapsed = std::chrono::duration<double>(capturedAt - lastVisual_).count();
        const bool consistent = lastVisual_ != Clock::time_point{} && scene == visualScene_ && elapsed <= .75 &&
            std::hypot(x - lastX_, y - lastY_) <= 120.0 * 1.205;
        lastVisual_ = capturedAt; lastFrame_ = frame; lastX_ = x; lastY_ = y; visualScene_ = scene;
        if (readoutActive_) {
            recoveryCount_ = consistent ? recoveryCount_ + 1 : 1;
            if (recoveryCount_ < 2) return false;
        }
        readoutActive_ = false; failing_ = false; recoveryCount_ = 0;
        return true;
    }
private:
    bool readoutActive_ = false, failing_ = false;
    int recoveryCount_ = 0;
    int visualScene_ = 0;
    std::uint64_t lastFrame_ = 0;
    double lastX_ = 0, lastY_ = 0;
    Clock::time_point lastVisual_{}, lastReadout_{};
    Clock::time_point readCapture_{};
    std::uint64_t readFrame_=0;
    int readScene_=0,readCount_=0;
    double readX_=0,readY_=0;
};
