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

// Keeps UI transitions separate from raw per-frame feature checks. A stable
// state requires two confirmed observations; suspected map UI must never hide
// gameplay overlays on its own. Missing HUD evidence needs a longer, one
// second confirmation window before it can clear minimap caches.
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
