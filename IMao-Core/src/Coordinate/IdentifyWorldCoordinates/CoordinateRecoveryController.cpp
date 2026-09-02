#include "CoordinateRecoveryController.h"

namespace {
constexpr auto kTrustedPositionHold = std::chrono::seconds(2);
}

void CoordinateRecoveryController::Reset() {
    state_ = CoordinateLockState::Uninitialized;
    visibleFrames_ = 0;
    consecutiveFailures_ = 0;
    failedRecognitionBatches_ = 0;
    hasTrustedPosition_ = false;
    lastTrustedAt_ = {};
}

void CoordinateRecoveryController::RestartRecovery() {
    Reset();
    OnRecognitionFailure();
}

void CoordinateRecoveryController::SetVisible(bool visible, Clock::time_point) {
    if (!visible) {
        state_ = CoordinateLockState::Hidden;
        visibleFrames_ = 0;
        return;
    }
    if (state_ == CoordinateLockState::Hidden) state_ = CoordinateLockState::Uninitialized;
    if (state_ == CoordinateLockState::Uninitialized) {
        if (++visibleFrames_ >= 2) state_ = CoordinateLockState::Recovering;
    }
}

void CoordinateRecoveryController::OnContinuitySuccess(Clock::time_point now) {
    state_ = CoordinateLockState::Tracking;
    consecutiveFailures_ = 0;
    failedRecognitionBatches_ = 0;
    hasTrustedPosition_ = true;
    lastTrustedAt_ = now;
}

void CoordinateRecoveryController::OnContinuityFailure(Clock::time_point) {
    if (state_ == CoordinateLockState::Tracking) {
        consecutiveFailures_ = 1;
        state_ = CoordinateLockState::Suspect;
    }
    else if (state_ == CoordinateLockState::Suspect) {
        if (++consecutiveFailures_ >= 3) state_ = CoordinateLockState::Recovering;
    }
}

void CoordinateRecoveryController::OnRecognitionSuccess(Clock::time_point now) {
    OnContinuitySuccess(now);
}

void CoordinateRecoveryController::OnRecognitionFailure() {
    state_ = CoordinateLockState::Recovering;
    ++failedRecognitionBatches_;
}

bool CoordinateRecoveryController::CanUseTrustedPosition(Clock::time_point now) const {
    return hasTrustedPosition_ && state_ != CoordinateLockState::Hidden &&
        now - lastTrustedAt_ <= kTrustedPositionHold;
}

bool CoordinateRecoveryController::ShouldHideMarkers(Clock::time_point now) const {
    if (state_ == CoordinateLockState::Hidden) return true;
    if (state_ == CoordinateLockState::Suspect || state_ == CoordinateLockState::Recovering) {
        return !CanUseTrustedPosition(now);
    }
    return false;
}

std::string_view CoordinateRecoveryController::StateName(CoordinateLockState state) {
    switch (state) {
    case CoordinateLockState::Uninitialized: return "Uninitialized";
    case CoordinateLockState::Tracking: return "Tracking";
    case CoordinateLockState::Suspect: return "Suspect";
    case CoordinateLockState::Recovering: return "Recovering";
    case CoordinateLockState::Hidden: return "Hidden";
    }
    return "Unknown";
}
