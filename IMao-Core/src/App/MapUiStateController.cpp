#include "MapUiStateController.h"

MapUiState MapUiStateController::Classify(const MapUiEvidence& evidence) {
    if (evidence.bigMapConfirmed) return MapUiState::BigMap;
    if (evidence.minimapVisible) return MapUiState::Gameplay;
    return MapUiState::Unknown;
}

MapUiStateUpdate MapUiStateController::Update(const MapUiEvidence& evidence) {
    MapUiStateUpdate update;
    update.previous = state_;
    update.current = state_;
    update.observed = Classify(evidence);

    if (update.observed == MapUiState::Unknown) {
        if (pending_ != MapUiState::Unknown) {
            pending_ = MapUiState::Unknown;
            pendingFrames_ = 1;
        }
        else {
            ++pendingFrames_;
        }
        // A capture can lose HUD features during an animation or one bad DXGI
        // frame. Treat a missing minimap as an actual HUD disappearance only
        // after ten observations. This protects localization state only;
        // current-frame visibility independently suppresses cached markers.
        if (state_ != MapUiState::Unknown && pendingFrames_ >= 10) {
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
