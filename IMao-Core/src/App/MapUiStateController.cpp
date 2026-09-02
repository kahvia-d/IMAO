#include "MapUiStateController.h"

MapUiState MapUiStateController::Classify(const MapUiEvidence& evidence) {
    if (evidence.compassVisible) return MapUiState::BigMap;
    if (evidence.minimapVisible) return MapUiState::Gameplay;
    return MapUiState::Unknown;
}

MapUiStateUpdate MapUiStateController::Update(const MapUiEvidence& evidence) {
    MapUiStateUpdate update;
    update.previous = state_;
    update.current = state_;
    update.observed = Classify(evidence);

    if (update.observed == MapUiState::Unknown) {
        pending_ = MapUiState::Unknown;
        pendingFrames_ = 0;
        if (state_ != MapUiState::Unknown) {
            state_ = MapUiState::Unknown;
            update.current = state_;
            update.changed = true;
        }
        return update;
    }

    if (pending_ != update.observed) {
        pending_ = update.observed;
        pendingFrames_ = 1;
    }
    else {
        ++pendingFrames_;
    }

    // A conflicting first frame hides the old overlay immediately while still
    // recording which UI is being entered.  App can therefore preserve its
    // player-location hint without treating an opening/closing animation as a
    // teleport.
    if (state_ == MapUiState::Gameplay && update.observed == MapUiState::BigMap) {
        state_ = MapUiState::EnteringBigMap;
        update.current = state_;
        update.changed = true;
        return update;
    }
    if (state_ == MapUiState::BigMap && update.observed == MapUiState::Gameplay) {
        state_ = MapUiState::LeavingBigMap;
        update.current = state_;
        update.changed = true;
        return update;
    }

    if ((state_ == MapUiState::EnteringBigMap && update.observed != MapUiState::BigMap) ||
        (state_ == MapUiState::LeavingBigMap && update.observed != MapUiState::Gameplay)) {
        state_ = MapUiState::Unknown;
        update.current = state_;
        update.changed = true;
        return update;
    }

    if (pendingFrames_ >= 2 && state_ != update.observed) {
        state_ = update.observed;
        update.current = state_;
        update.changed = true;
    }
    return update;
}

void MapUiStateController::Reset() {
    state_ = MapUiState::Unknown;
    pending_ = MapUiState::Unknown;
    pendingFrames_ = 0;
}

const char* MapUiStateController::StateName(MapUiState state) {
    switch (state) {
    case MapUiState::Unknown: return "Unknown";
    case MapUiState::Gameplay: return "Gameplay";
    case MapUiState::EnteringBigMap: return "EnteringBigMap";
    case MapUiState::BigMap: return "BigMap";
    case MapUiState::LeavingBigMap: return "LeavingBigMap";
    }
    return "Unknown";
}
