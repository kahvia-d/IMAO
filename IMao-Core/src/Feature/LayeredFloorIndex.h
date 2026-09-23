#pragma once

#include "Match/FeatureMatch.h"

#include <filesystem>
#include <string>
#include <vector>

// Which floor of a layered map ("分层地图") the player is standing in.
//
// The region pack cannot answer this. Several floors share one tile coordinate, so every
// appearance sits at the same coordinates and the visual index merges them; the pack knows
// the position but not the layer. The floor is identifiable from the imagery though - on a
// real 叩天关 minimap the correct floor's own composite scored 21 near-anchor matches where
// the other floors scored 0-6 - so scripts/New-LayeredFloorIndex.ps1 writes one small
// descriptor set per floor and this votes on the live minimap.
//
// The index is optional. A pack without one is not an error: callers simply keep treating
// every layered marker the same way they do today.
namespace LayeredFloors {

/// One tile of a floor, with a coarse "the player is standing inside this floor's cave" mask:
/// the overlay's alpha downsampled to a gridSize x gridSize bit grid, packed as hex.
struct FloorTile {
    int x = 0;
    int y = 0;
    std::string occupancy; // gridSize*gridSize bits, big-endian nibbles, row-major
};

/// The index's coordinate transform, needed to turn a runtime map coordinate into a tile
/// pixel so the occupancy grids can be queried.
struct Transform {
    double originX = 0.0;
    double originY = 0.0;
    double scale = 1.0;
    double virtualMapSize = 850.0;
    double tileSize = 1024.0;
    int gridSize = 64;
};

struct FloorEntry {
    // The layered map this floor belongs to (the denominator of floorId): 叩天关 is 1,
    // 环木阙 2, 眠龙庭 3. Two different layered maps can occupy the same tile coordinate,
    // which is why "above/below" only means anything inside one of them.
    int layerId = 0;
    std::string floorId;   // "-1/1"
    std::string layerName; // "叩天关"
    std::string floorName; // "叩天关·上层"
    int level = 0;         // "-1": lower (more negative) is deeper
    ImageFeatureData features;
    std::vector<FloorTile> tiles;
    // Centre of the floor's footprint in map coordinates. A cold start has no position at
    // all, and this is the only coordinate the layered index can offer to scope a search.
    double centerMapX = 0.0;
    double centerMapY = 0.0;
    bool hasCenter = false;
};

struct Index {
    std::vector<FloorEntry> floors;
    Transform transform;
};

struct FloorVote {
    int layerId = 0;
    std::string floorId;
    int matches = 0;
    // Index of this floor in the vector handed to Classify. Two regions can name a floor the
    // same way, so a caller that wants to report *which* region won cannot look the id up again.
    std::size_t sourceIndex = 0;
};

struct Classification {
    bool identified = false;
    int layerId = 0;
    std::string floorId;
    int winnerMatches = 0;
    int runnerUpMatches = 0;
    // Descending by matches; kept in full so a caller can log why it decided what it did.
    std::vector<FloorVote> votes;
};

/// Reads <packDirectory>/layered-floors/floor-index.json plus the .imf files it names.
/// Returns false only on a malformed index; a missing index is (false, ...) with `error`
/// describing it, and the caller decides whether that matters.
///
/// `maxKeypointsPerFloor` caps the descriptors kept per floor (0 = keep all). The index only
/// ever answers "which floor is this", never "where exactly", so a few hundred descriptors
/// carry the same decision at a fraction of the cost: with 90 floors across the game, keeping
/// everything made one classification roughly ten times slower and the indexes ten times
/// larger.
bool Load(const std::filesystem::path& packDirectory, Index& index, std::string& error,
    int maxKeypointsPerFloor = 0);

/// True when the map coordinate falls inside this floor's cave. One coordinate can be inside
/// several floors' footprints only where two caves overlap, and it is inside none of them out
/// on the surface - but note that standing on the surface ABOVE a cave shares the coordinate,
/// so containment can never decide "am I in a layer", only "which one".
bool Contains(const FloorEntry& floor, const Transform& transform, double mapX, double mapY);

/// Votes every floor against the query descriptors and returns the winner. `identified` is
/// false when the winner has too few matches or does not lead the runner-up by `margin`,
/// which is the common case out in the open; callers must treat that as "unknown", never as
/// a reason to guess.
///
/// The defaults are calibrated against real frames (2026-09-23, jinzhou, via
/// IMaoLayeredFloorProbe): two in-layer captures vote 22 for the correct floor with a
/// runner-up of 3, while nine surface captures peak at 6 and never lead by 2x. 10 sits
/// between the two populations - below 8 the classifier started accepting surface frames.
Classification Classify(const ImageFeatureData& query, const std::vector<const FloorEntry*>& floors,
    int minimumMatches = 10, double margin = 2.0, float ratio = 0.75f, float maxDistance = 0.6f);

/// Convenience overload for callers that hold the floors by value. The pointer form above is the
/// real one: a FloorEntry owns its descriptors, so passing a vector of them by value copies tens
/// of megabytes per call once every floor in the game is a candidate.
inline Classification Classify(const ImageFeatureData& query, const std::vector<FloorEntry>& floors,
    int minimumMatches = 10, double margin = 2.0, float ratio = 0.75f, float maxDistance = 0.6f) {
    std::vector<const FloorEntry*> pointers;
    pointers.reserve(floors.size());
    for (const auto& floor : floors) pointers.push_back(&floor);
    return Classify(query, pointers, minimumMatches, margin, ratio, maxDistance);
}

/// "-2/3" -> -2. The numerator orders the floors inside one layered map.
int FloorLevel(const std::string& floorId);

/// "-2/3" -> 3, the layered map the floor belongs to. 0 when the id is malformed.
int FloorLayerId(const std::string& floorId);

}
