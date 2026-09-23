// Pins the marker-role mapping, which is easy to get backwards: a point's `floorId` names
// the layered map ("1" = 叩天关) while its `level` names the floor inside it ("-2/1"), and
// `layer::stateId` is the Kuro state (8) rather than the runtime scene id (1). Two earlier
// revisions mixed those up and silently drew every marker as if no floor were known.

#include "Runtime/LayeredMapState.h"
#include "Runtime/NearbySelection.h"

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

        // The nearby hotkeys act on what the player can see. While standing in a layered map the
        // surface markers are hidden, so a key press must not complete one of them - and it must
        // not complete a chest from another floor either, which the player cannot have reached:
        // standing on 1楼 may not tick off the chest on 4楼.
        {
            LayeredMap::Snapshot state;
            state.active = true;
            state.kuroStateId = 902;     // 下层金库
            state.layerId = 15;          // 贵金属与艺术品藏区
            state.floorId = "-1/15";     // 1楼
            state.level = -1;
            state.heightDirection = -1;  // 1楼 at the bottom, 4楼 on top
            LayeredMap::SetForTest(state);

            const auto candidate = [](const std::string& mapId, const std::string& floorId, double pixels) {
                NearbySelection::Candidate item;
                item.item.layer.stateId = 902;
                item.item.layer.floorId = mapId;
                item.item.layer.level = floorId;
                item.item.itemId = mapId + floorId;
                item.distance = pixels;
                item.screenDistance = pixels;
                return item;
            };
            const auto here = candidate("15", "-1/15", 5);
            const auto topFloor = candidate("15", "-4/15", 5);
            const auto surface = candidate("", "", 1);
            const auto elsewhere = candidate("16", "-1/16", 5);

            Require(NearbySelection::Includes(here, NearbySelection::Intent::Complete),
                "the floor the player stands on must stay completable");
            Require(!NearbySelection::Includes(topFloor, NearbySelection::Intent::Complete),
                "another floor's chest must not be completable by the key");
            Require(NearbySelection::Includes(topFloor, NearbySelection::Intent::Guide),
                "reading another floor's guide is still allowed");
            Require(!NearbySelection::Includes(surface, NearbySelection::Intent::Complete),
                "a hidden surface marker must not be completable by the key");
            Require(!NearbySelection::Includes(surface, NearbySelection::Intent::Guide),
                "a hidden surface marker must not be reachable by the key at all");
            Require(!NearbySelection::Includes(elsewhere, NearbySelection::Intent::Guide),
                "another layered map's marker is hidden too");

            // Out in the open nothing is hidden, so the same marker is completable again.
            LayeredMap::SetForTest({});
            Require(NearbySelection::Includes(surface, NearbySelection::Intent::Complete),
                "leaving the layer must restore surface completion");
        }

        std::cout << "Layered marker role tests passed\n";
        return 0;    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
