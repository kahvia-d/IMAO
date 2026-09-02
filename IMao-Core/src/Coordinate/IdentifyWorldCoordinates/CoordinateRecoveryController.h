#pragma once

#include <chrono>
#include <string_view>

enum class CoordinateLockState {
    Uninitialized,
    Tracking,
    Suspect,
    Recovering,
    Hidden
};

class CoordinateRecoveryController {
public:
    using Clock = std::chrono::steady_clock;

    void Reset();
    // A discontinuity such as teleporting must discard the previously trusted
    // position before requesting a new global visual lock.
    void RestartRecovery();
    void SetVisible(bool visible, Clock::time_point now = Clock::now());
    void OnContinuitySuccess(Clock::time_point now = Clock::now());
    void OnContinuityFailure(Clock::time_point now = Clock::now());
    void OnRecognitionSuccess(Clock::time_point now = Clock::now());
    void OnRecognitionFailure();

    CoordinateLockState State() const { return state_; }
    bool ShouldRequestRecognition() const { return state_ == CoordinateLockState::Recovering; }
    bool CanUseTrustedPosition(Clock::time_point now = Clock::now()) const;
    bool ShouldHideMarkers(Clock::time_point now = Clock::now()) const;
    bool ShouldSearchAllScenes() const { return failedRecognitionBatches_ >= 3; }
    int ConsecutiveFailures() const { return consecutiveFailures_; }
    int FailedRecognitionBatches() const { return failedRecognitionBatches_; }
    static std::string_view StateName(CoordinateLockState state);

private:
    CoordinateLockState state_ = CoordinateLockState::Uninitialized;
    int visibleFrames_ = 0;
    int consecutiveFailures_ = 0;
    int failedRecognitionBatches_ = 0;
    bool hasTrustedPosition_ = false;
    Clock::time_point lastTrustedAt_{};
};
