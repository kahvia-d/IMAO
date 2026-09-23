#pragma once

#include "../Domain/MapData.h"
#include "../Feature/LayeredFloorIndex.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Which floor of a layered map ("分层地图") the player is standing in, and what that means
// for one marker.
//
// Layers share the surface's coordinates, so "am I in a layer" is not a position test: the
// per-floor classifier reads the minimap imagery (LayeredFloorIndex) and this holds its
// answer, debounced so one odd frame cannot flip the whole marker display.
namespace LayeredMap {

/// What to draw for one marker.
enum class MarkerRole {
    /// No floor known: everything draws as before, layered markers keep the layered badge.
    Normal,
    /// A floor is known and this marker is not part of it: do not draw it at all.
    Hidden,
    /// The floor the player is standing on: normal icon, no layered badge.
    Current,
    /// Another floor of the same layered map, higher up: dimmed with a warm up marker.
    Above,
    /// Another floor of the same layered map, lower down: dimmed with a cool down marker.
    Below,
};

struct Snapshot {
    bool active = false;
    /// The runtime scene (World = 1). Index entries are filtered by this.
    int sceneId = 0;
    /// The Kuro state the floor's tiles come from (World = 8). Markers carry THIS in
    /// ItemDatas::layer::stateId, not the scene id, so the two must not be confused: doing
    /// so silently matched nothing and every layered rule was skipped.
    int kuroStateId = 0;
    /// The layered map the floor belongs to (叩天关 = 1). Two layered maps can share a tile
    /// coordinate, so "above/below" only means anything within one of them.
    int layerId = 0;
    std::string floorId; // "-2/1"
    int level = 0;       // -2
    /// Which way this layered map's levels run; see FloorEntry::heightDirection.
    int heightDirection = 1;
    std::uint64_t revision = 0;
    /// Floors the imagery cannot tell apart from `floorId`.
    ///
    /// Inside 下层金库's 贵金属与艺术品藏区 the four floors are the same marble hall: their
    /// composites share one dimmed surface base and the interiors carry few distinctive
    /// features, so the votes come out 12/10/5/4 and the required 2x lead never appears - the
    /// layer state stayed off entirely. Containment already says all four caves hold the player,
    /// so the honest answer is "one of these", not "this one": everything on a floor in this set
    /// draws as the current floor instead of being asserted above or below.
    std::vector<std::string> equivalentFloorIds;
};

/// Where to point a search when the tool has no position at all yet.
///
/// A cold start has no prior, so the localizer sweeps the whole map; inside a cave that sweep
/// finds about eight mutual matches and fails geometric verification. The layered index does
/// know where each floor's cave is, and the same minimap frame says which floor it is, so a
/// cold start can be scoped to that floor instead of the entire map. It is a search scope, not
/// a position: nothing is published from it, and a wrong guess only costs one bounded search.
struct Scope {
    bool valid = false;
    int sceneId = 0;
    int layerId = 0;
    std::string floorId;
    double mapX = 0.0;
    double mapY = 0.0;
};

/// Loads every <featureDataRoot>/KuroTilePacks/<region>/layered-floors index. A pack
/// without one is normal - most regions have no layered maps - and is simply skipped.
void Install(const std::filesystem::path& featureDataRoot);

/// Classifies one minimap capture and debounces the answer into the state.
///
/// `sceneId` is 0 before the first localisation: that is the cold start, and there the call
/// only refreshes the search scope (see ScopeHint) because there is no position to interpret
/// a floor against. Once a scene is known, `mapX`/`mapY` are the player's map coordinate and
/// decide whether the player is still standing in the known floor's cave, which is what keeps
/// the answer stable where the imagery alone is too weak to re-confirm it every frame.
void ObserveMinimap(const ImageFeatureData& minimapFeatures, int sceneId, double mapX, double mapY);

/// The current cold-start search scope; invalid once a scene is known.
Scope ScopeHint();

Snapshot Read();

/// The role of one marker under the current state. Used by both maps: they share the icon
/// drawing entry point.
MarkerRole RoleFor(const ItemDatas& item);

/// For tests and for tearing the app down.
void Reset();

/// Test hook: installs a state directly, so the marker-role mapping (which id field means
/// what) can be pinned by a unit test instead of only by playing the game.
void SetForTest(const Snapshot& snapshot);

}

