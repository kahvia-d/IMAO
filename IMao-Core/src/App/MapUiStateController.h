#pragma once

enum class MapUiState {
    Unknown,
    Gameplay,
    EnteringBigMap,
    BigMap,
    LeavingBigMap
};

struct MapUiEvidence {
    // A colour-only compass hit is deliberately not sufficient.  App sets
    // this only after the candidate has persisted and the map canvas passed a
    // structural verification.
    bool bigMapConfirmed = false;
    bool minimapVisible = false;
};

struct MapUiStateUpdate {
    MapUiState previous = MapUiState::Unknown;
    MapUiState current = MapUiState::Unknown;
    MapUiState observed = MapUiState::Unknown;
    bool changed = false;
};

// One frame's big-map inputs.  Kept beside the state machine so the rule the detectors feed is written
// once and can be tested without a live game.
struct MapFrameEvidence {
    bool controlsVisible = false;             // both zoom-control layouts
    bool compassVisible = false;              // the colour probe
    bool compassVerified = false;             // the template match: the widget itself
    bool structureConfirmed = false;          // the last canvas verification
    bool structureRequiresControls = false;   // that verification happened with the zoom controls up
    bool minimapAbsentLongEnough = false;
    bool minimapVisible = false;

    // A template-verified compass is the widget, and the widget only exists on the full-screen map, so
    // it authorises on its own - that is what keeps the map alive while the zoom strip is hidden and the
    // canvas verification cannot run.  The colour-only probe still needs the structural confirmation,
    // and loses its vote once the map was confirmed with the zoom controls visible.
    bool Probed() const {
        return controlsVisible || compassVerified ||
            (compassVisible && structureConfirmed && !structureRequiresControls);
    }
};

// The state machine requires the minimap HUD to have been missing for its absence interval; the marker
// policy reacts to the frame in front of it.
inline bool BigMapEvidence(const MapFrameEvidence& evidence) {
    return evidence.minimapAbsentLongEnough && evidence.Probed();
}
inline bool BigMapMarkersVisible(const MapFrameEvidence& evidence) {
    return !evidence.minimapVisible && evidence.Probed();
}

// Keeps UI transitions separate from raw per-frame feature checks. A stable
// state requires two confirmed observations. Missing HUD evidence keeps the
// localization session for ten observations, so an animation does not destroy
// useful position hints. OverlayVisibilityPolicy independently hides markers
// on the first missing observation instead of waiting for this debounce.
class MapUiStateController {
public:
    MapUiStateUpdate Update(const MapUiEvidence& evidence);
    void Reset();

    MapUiState State() const { return state_; }
    static const char* StateName(MapUiState state);
    static bool IsStableGameplay(MapUiState state) { return state == MapUiState::Gameplay; }
    static bool IsStableBigMap(MapUiState state) { return state == MapUiState::BigMap; }

private:
    static MapUiState Classify(const MapUiEvidence& evidence);

    MapUiState state_ = MapUiState::Unknown;
    MapUiState pending_ = MapUiState::Unknown;
    int pendingFrames_ = 0;
};
