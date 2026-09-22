// Pins the marker-role mapping, which is easy to get backwards: a point's `floorId` names
// the layered map ("1" = 叩天关) while its `level` names the floor inside it ("-2/1"), and
// `layer::stateId` is the Kuro state (8) rather than the runtime scene id (1). Two earlier
// revisions mixed those up and silently drew every marker as if no floor were known.

#include "Runtime/LayeredMapState.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using LayeredMap::MarkerRole;

void Require(bool result, const char* reason) {
    if (!result) throw std::runtime_error(reason);
}

ItemDatas Marker(int kuroStateId, const std::string& mapId, const std::string& level) {
    ItemDatas item;
    item.itemId = "test";
    item.nameId = "test";
    item.layer.stateId = kuroStateId;
    item.layer.floorId = mapId;
    item.layer.level = level;
    return item;
}

LayeredMap::Snapshot State(int layerId, const std::string& floorId, int level) {
    LayeredMap::Snapshot snapshot;
    snapshot.active = true;
    snapshot.sceneId = 1;      // World
    snapshot.kuroStateId = 8;  // ... and the Kuro state World markers carry
    snapshot.layerId = layerId;
    snapshot.floorId = floorId;
    snapshot.level = level;
    return snapshot;
}

} // namespace

int main() {
    try {
        // No floor known: nothing changes, layered markers keep their badge.
        LayeredMap::SetForTest({});
        Require(LayeredMap::RoleFor(Marker(8, "1", "-1/1")) == MarkerRole::Normal,
            "an unknown floor must not hide anything");
        Require(LayeredMap::RoleFor(Marker(8, "", "")) == MarkerRole::Normal,
            "an unknown floor must leave surface markers alone");

        // Standing on 叩天关·中层: layered map 1, floor "-2/1".
        LayeredMap::SetForTest(State(1, "-2/1", -2));
        Require(LayeredMap::RoleFor(Marker(8, "", "")) == MarkerRole::Hidden,
            "a surface collectible must hide while inside a layer");
        Require(LayeredMap::RoleFor(Marker(8, "1", "-2/1")) == MarkerRole::Current,
            "the floor the player is on is current");
        Require(LayeredMap::RoleFor(Marker(8, "1", "-1/1")) == MarkerRole::Above,
            "floor -1 sits above floor -2");
        Require(LayeredMap::RoleFor(Marker(8, "1", "-3/1")) == MarkerRole::Below,
            "floor -3 sits below floor -2");
        Require(LayeredMap::RoleFor(Marker(8, "2", "-3/2")) == MarkerRole::Hidden,
            "a different layered map sharing the tile is not above or below");
        Require(LayeredMap::RoleFor(Marker(8, "1", "-1000000/1")) == MarkerRole::Hidden,
            "an entrance marker sits on the surface");
        Require(LayeredMap::RoleFor(Marker(8, "1", "0")) == MarkerRole::Hidden,
            "a point with no floor sits on the surface");
        Require(LayeredMap::RoleFor(Marker(900, "1", "-1/1")) == MarkerRole::Normal,
            "a marker from another Kuro state is not this state's business");

        // Leaving the layer restores the plain display.
        LayeredMap::SetForTest({});
        Require(LayeredMap::RoleFor(Marker(8, "1", "-1/1")) == MarkerRole::Normal,
            "clearing the floor must restore the badge behaviour");

        std::cout << "Layered marker role tests passed\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
