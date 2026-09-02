#pragma once

enum class MapUiState {
    Unknown,
    Gameplay,
    EnteringBigMap,
    BigMap,
    LeavingBigMap
};

struct MapUiEvidence {
    bool compassVisible = false;
    bool minimapVisible = false;
};

struct MapUiStateUpdate {
    MapUiState previous = MapUiState::Unknown;
    MapUiState current = MapUiState::Unknown;
    MapUiState observed = MapUiState::Unknown;
    bool changed = false;
};

// Keeps UI transitions separate from the raw per-frame feature checks.  A
// transition must be observed twice, while a conflicting/unknown frame hides
// overlays immediately so stale map markers cannot leak onto gameplay.
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
