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
    /// Same layout, but set where the layer reused the surface tile's own pixels instead of
    /// drawing its own art. A layered map can take a piece of the surface as its own ground -
    /// 下层金库's 贵金属与艺术品藏区 draws the plaza in front of the building by copying it - and
    /// a player standing there is on the surface, not inside the layer, even though the layer's
    /// art covers the spot and its imagery matches. Empty on an index built before this grid
    /// existed, which simply means "nothing is shared".
    std::string shared;
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
    /// +1 when a bigger level is higher, -1 when the level runs the other way. Upstream uses two
    /// naming conventions and their levels run opposite ways (see LayerHeightDirection), so the
    /// above/below markers must not assume one of them.
    int heightDirection = 1;
    /// Where the floor sits inside its layered map, ascending with height; bigger is higher.
    ///
    /// Read off the floor's NAME (AssignHeightRanks) and preferred over `level * heightDirection`,
    /// because a level does not always follow the height: 黯原's 虚妄摇篮 is 二层 (-3) at the
    /// bottom, 入口 (-1) in the middle and 一层 (-2) on top. Floors whose names state nothing keep
    /// the level comparison, which is what the whole game used before the names were read.
    int heightRank = 0;
    ImageFeatureData features;
    /// A capped copy of `features`, used only when every floor in the game is a candidate.
    ///
    /// The cold start compares all 90 floors and only needs an answer good enough to scope a
    /// search, so it can afford a sample. The vote that DECIDES a floor runs against a handful
    /// of floors that containment already narrowed down, and there the full set matters: on two
    /// real 下层金库 captures a 600-descriptor sample picks the wrong floor (13 vs 13, 11 vs 10)
    /// where the full set picks 贵金属与艺术品藏区1楼 at 17 vs 8 and 15 vs 6.
    ImageFeatureData sampleFeatures;
    std::vector<FloorTile> tiles;
    /// How much of this floor's own art is the surface's art instead: shared cells / opaque cells
    /// over the whole floor (0 on an index built before the shared grid existed).
    ///
    /// Reported, not decided on: it is what says whether upstream drew this map FROM the surface
    /// drawing (拉海's 星炬学院 floors are 0.51-0.84, every other floor at most 0.32), which is
    /// worth seeing when a floor's above/below markers look wrong. What decides whether the
    /// comparison may be used at all is AdjacentToSurface.
    double copiedFraction = 0.0;
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
/// The cold start, where every floor in the game is a candidate, votes against `sampleFeatures`
/// (see FloorEntry); `maxKeypointsPerFloor` is the cap for that copy, 0 disabling it. The full
/// `features` set is always loaded: it is what the deciding vote uses.
bool Load(const std::filesystem::path& packDirectory, Index& index, std::string& error,
    int maxKeypointsPerFloor = 0);

/// True when the map coordinate falls inside this floor's cave. One coordinate can be inside
/// several floors' footprints only where two caves overlap, and it is inside none of them out
/// on the surface - but note that standing on the surface ABOVE a cave shares the coordinate,
/// so containment can never decide "am I in a layer", only "which one".
bool Contains(const FloorEntry& floor, const Transform& transform, double mapX, double mapY);

/// True when the coordinate is inside the floor's art with `marginCells` of art around it, i.e.
/// standing on drawn art rather than beside it.
///
/// NOT an "am I inside the cave" test: the layer art is a drawing, so the rooms inside a floor -
/// 虚妄摇篮's 一层, every room of 下层金库 - are transparent holes in it and read as "not inside".
/// Contains, which keeps a cell of slack, is what the runtime uses for containment.
bool InsideWithMargin(const FloorEntry& floor, const Transform& transform, double mapX, double mapY,
    int marginCells = 1);

/// How much of the layered art around this coordinate is the surface's own pixels rather than the
/// layer's own drawing: the fraction of opaque cells in a (2*radiusCells+1) square that the tile
/// marks as shared. A layered map can adopt a piece of the surface as its own ground (下层金库's
/// 贵金属与艺术品藏区 draws the plaza in front of the building by copying it), and a player
/// standing on that piece is on the surface - the layer covers the spot and its imagery matches,
/// so nothing but this comparison can say so.
///
/// Measured with radius 3 on 下层金库: the shared plaza reads 0.67-0.69, the hall inside the
/// building 0.16-0.34, so a threshold of 0.5 sits between them with roughly a 2x margin.
double SharedFraction(const FloorEntry& floor, const Transform& transform, double mapX, double mapY,
    int radiusCells = 3);

/// True when a floor can share ground with the surface at all, which only the floor next to the
/// surface can. Everything above or below it is a different place, so art it copied from the
/// surface drawing (拉海's 星炬学院 floors are 51-84% the surface's own pixels) is NOT surface
/// ground, and acting on the comparison there would hide the markers the player is standing on.
/// Measured over all 90 floors: only 拉海's 星炬学院 and 日树 floors copy more than 32% of their art,
/// and their level -2/-3 floors do it while their level -1 floor is the one at ground level.
bool AdjacentToSurface(const FloorEntry& floor);

/// True when a floor's art is mostly the surface's own drawing, which makes SharedFraction
/// meaningless for it: the layer drew its whole ground by copying the surface, so "this spot looks
/// like the surface" is true of the layer's OWN ground too.
///
/// 星炬学院 is the case that proved it: all four of its floors are 58-84% the surface's pixels
/// (against at most 32% for every other floor in the game), and a player standing in its 广场区 is
/// inside the academy - the field log has `shared=1 fraction=1.0` there and the surface markers
/// coming back, which is exactly what the player did not want.
bool ArtIsSurfaceCopy(const FloorEntry& floor, double gate = 0.5);

/// True when the shared-ground comparison may be used for this floor at all: it has to be the floor
/// next to the surface, and its art must not be a redrawing of the surface's own.
///
/// 下层金库's plaza answers yes on both (1楼, 27-32% copied) and is why the comparison exists at
/// all; 星炬学院's 广场区 is the floor next to the surface (level -1) but answers no on the second,
/// because its ground is the layer's own and the surface's markers do not belong on it.
bool SharesSurfaceGround(const FloorEntry& floor);

/// Votes every floor against the query descriptors and returns the winner. `identified` is
/// false when the winner has too few matches or does not lead the runner-up by `margin`,
/// which is the common case out in the open; callers must treat that as "unknown", never as
/// a reason to guess.
///
/// The defaults are calibrated against real frames (2026-09-23, jinzhou, via
/// IMaoLayeredFloorProbe): two in-layer captures vote 22 for the correct floor with a
/// runner-up of 3, while nine surface captures peak at 6 and never lead by 2x. 10 sits
/// between the two populations - below 8 the classifier started accepting surface frames.
/// `useSamples` votes against each floor's capped `sampleFeatures` instead of the full set, which
/// is what the cold start does: it compares every floor in the game and only needs enough to
/// scope a search.
Classification Classify(const ImageFeatureData& query, const std::vector<const FloorEntry*>& floors,
    int minimumMatches = 10, double margin = 2.0, float ratio = 0.75f, float maxDistance = 0.6f,
    bool useSamples = false);

/// Convenience overload for callers that hold the floors by value. The pointer form above is the
/// real one: a FloorEntry owns its descriptors, so passing a vector of them by value copies tens
/// of megabytes per call once every floor in the game is a candidate.
inline Classification Classify(const ImageFeatureData& query, const std::vector<FloorEntry>& floors,
    int minimumMatches = 10, double margin = 2.0, float ratio = 0.75f, float maxDistance = 0.6f,
    bool useSamples = false) {
    std::vector<const FloorEntry*> pointers;
    pointers.reserve(floors.size());
    for (const auto& floor : floors) pointers.push_back(&floor);
    return Classify(query, pointers, minimumMatches, margin, ratio, maxDistance, useSamples);
}

/// "-2/3" -> -2. The numerator orders the floors inside one layered map.
int FloorLevel(const std::string& floorId);

/// Whether a bigger level means a higher floor for this layered map: +1 normally (叩天关's
/// 上层(-1) above 下层(-3)), -1 where the names number the floors the other way round
/// (下层金库's 1楼(-1) below 4楼(-4)). Derived from the names, not assumed.
int LayerHeightDirection(const std::vector<FloorEntry>& floors, int layerId);

/// Fills in both `heightDirection` and `heightRank` for every floor; Load calls this once. Ranks
/// are used in preference to the direction because not every layered map's heights follow its
/// levels: 虚妄摇篮's 入口 sits between 二层 and 一层, which no single direction can reproduce.
void AssignHeightRanks(std::vector<FloorEntry>& floors);

/// The rank `AssignHeightRanks` gave this floor, or 0 when the index has no such floor.
int HeightRank(const std::vector<FloorEntry>& floors, const std::string& floorId);

/// Where a marker on the second floor sits relative to a player on the first: -1 below, +1 above.
///
/// The ranks decide it while both are known, because a level does not always follow the height
/// (黯原's 虚妄摇篮 is 二层 at the bottom, 入口 in the middle, 一层 on top - the player on 入口 has
/// 一层 above and 二层 below, which no direction can express). `direction` is the fallback that the
/// whole game used before the names were read: +1 meaning a bigger level is higher.
int HeightComparison(int currentRank, int currentLevel, int markerRank, int markerLevel, int direction);
/// "-2/3" -> 3, the layered map the floor belongs to. 0 when the id is malformed.
int FloorLayerId(const std::string& floorId);

}
