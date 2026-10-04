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

// A render miss, absent capture or focus loss is not evidence that the map was
// closed. Require the current captured HUD as well as the debounced UI state.
inline bool ConfirmedMapClosed(MapUiState state, bool minimapVisible, bool displayContext, bool captureFresh) {
    return state == MapUiState::Gameplay && minimapVisible && displayContext && captureFresh;
}

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
    // An independently confirmed big-map viewport fix arrived seconds ago. Matching the central canvas
    // against the map's own features is the strongest evidence that the surface in front of the player
    // *is* the map, and unlike the HUD probes it does not blink: the probes read a widget, so a hidden
    // zoom strip, a covered compass or a hue that drifts with the terrain behind a translucent ring all
    // look like "no map". The 2026-09-28 player log is that failure mode at full strength - the probes
    // went blind for one to two seconds at a time while the canvas kept matching at 74-100 inliers and
    // the player never left the map.
    bool anchorFresh = false;

    // Task-icon descriptors can match map labels during a pan. Current,
    // verified full-screen widgets outweigh that weak HUD hit. An old canvas
    // anchor must not suppress genuine gameplay after the map actually closes.
    bool GameplayHudVisible() const {
        return minimapVisible && !controlsVisible && !compassVerified;
    }

    // A template-verified compass is the widget, and the widget only exists on the full-screen map, so
    // it authorises on its own - that is what keeps the map alive while the zoom strip is hidden and the
    // canvas verification cannot run.  The colour-only probe still needs the structural confirmation,
    // and loses its vote once the map was confirmed with the zoom controls visible.  A live viewport
    // anchor stands with the verified compass: the entry probes say the map was opened, the anchor says
    // it is still there.
    bool Probed() const {
        return controlsVisible || compassVerified || anchorFresh ||
            (compassVisible && structureConfirmed && !structureRequiresControls);
    }
};

// Colour/anchor candidates require the HUD absence interval; current verified
// map widgets stand on their own. Marker visibility uses the current surface.
inline bool BigMapEvidence(const MapFrameEvidence& evidence) {
    return evidence.controlsVisible || evidence.compassVerified ||
        (evidence.minimapAbsentLongEnough && evidence.Probed());
}
inline bool BigMapMarkersVisible(const MapFrameEvidence& evidence) {
    return !evidence.GameplayHudVisible() && evidence.Probed();
}

// Keeps UI transitions separate from raw per-frame feature checks. A stable
// state requires two confirmed observations. Missing HUD evidence keeps the
// localization session for ten observations, so an animation does not destroy
// useful position hints. OverlayVisibilityPolicy answers the frame in front of
// it - it holds a published surface only for the short evidence grace below and
// revokes it as soon as the capture carries evidence for the other surface.
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
