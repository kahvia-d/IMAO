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
    int sceneId = 0;
    /// The layered map the floor belongs to (叩天关 = 1). Two layered maps can share a tile
    /// coordinate, so "above/below" only means anything within one of them.
    int layerId = 0;
    std::string floorId; // "-2/1"
    int level = 0;       // -2
    std::uint64_t revision = 0;
};

/// Loads every <featureDataRoot>/KuroTilePacks/<region>/layered-floors index. A pack
/// without one is normal - most regions have no layered maps - and is simply skipped.
void Install(const std::filesystem::path& featureDataRoot);

/// Classifies one minimap capture and debounces the answer into the state. Cheap enough to
/// call per capture, but it rate-limits itself, so callers do not need to.
void ObserveMinimap(const ImageFeatureData& minimapFeatures, int sceneId);

Snapshot Read();

/// The role of one marker under the current state. Used by both maps: they share the icon
/// drawing entry point.
MarkerRole RoleFor(const ItemDatas& item);

/// For tests and for tearing the app down.
void Reset();

}
