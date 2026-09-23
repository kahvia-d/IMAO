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

// A single-tile floor whose occupancy covers the whole tile and whose left half is marked as
// ground the layered map copied from the surface. Used to pin SharedFraction: the left half must
// read as shared and the right half as the layer's own art.
LayeredFloors::FloorEntry SharedGroundFloor() {
    LayeredFloors::FloorEntry floor;
    floor.layerId = 15;
    floor.floorId = "-1/15";
    floor.floorName = "贵金属与艺术品藏区1楼";
    floor.level = -1;
    const int grid = 64;
    const int bits = grid * grid;
    std::vector<int> occupancy(bits, 1), shared(bits, 0);
    for (int y = 0; y < grid; ++y) {
        for (int x = 0; x < grid / 2; ++x) shared[y * grid + x] = 1;
    }
    const auto pack = [&](const std::vector<int>& source) {
        std::string hex;
        for (int i = 0; i < bits; i += 4) {
            hex += "0123456789abcdef"[source[i] | (source[i + 1] << 1) | (source[i + 2] << 2) |
                (source[i + 3] << 3)];
        }
        return hex;
    };
    LayeredFloors::FloorTile tile;
    tile.x = 3;
    tile.y = 0;
    tile.occupancy = pack(occupancy);
    tile.shared = pack(shared);
    floor.tiles.push_back(std::move(tile));
    return floor;
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

LayeredMap::Snapshot SharedState(int layerId, const std::string& floorId, int level) {
    auto snapshot = State(layerId, floorId, level);
    snapshot.sharedGround = true;
    snapshot.heightDirection = -1; // 下层金库's 1楼 sits at the bottom, its 4楼 on top
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

            // On shared ground the surface marker is a candidate again, because the marker role
            // itself is what decides this - see the role test below.
            LayeredMap::SetForTest(SharedState(15, "-1/15", -1));
            Require(NearbySelection::Includes(surface, NearbySelection::Intent::Complete),
                "a surface marker on shared ground must be completable by the key");
            Require(NearbySelection::Includes(here, NearbySelection::Intent::Complete),
                "the floor's own marker stays completable on shared ground");
        }

        // Ground a layered map copied from the surface reads as shared, and ground it drew itself
        // does not. The runtime uses this to show the surface's own markers again where the layer
        // only drew the surface's art as its ground, instead of hiding one set of markers or the
        // other: 下层金库's plaza carries the surface's markers while the vault's own markers sit
        // a few units inside the door.
        {
            using LayeredFloors::SharedFraction;
            LayeredFloors::Transform transform;   // the World defaults are enough for the geometry
            const auto floor = SharedGroundFloor();
            // Tile (3,0) pixel -> map coordinate: the inverse of the mapping the index stores.
            const auto mapPointOf = [&](double pixelX, double pixelY) {
                const double gameX = (3.0 * transform.tileSize + pixelX - transform.tileSize) *
                    transform.virtualMapSize / transform.tileSize;
                const double gameY = pixelY * transform.virtualMapSize / transform.tileSize;
                return std::pair<double, double>{ gameX * transform.scale + transform.originX,
                    gameY * transform.scale + transform.originY };
            };
            const auto left = mapPointOf(256.0, 512.0);    // shared half
            const auto right = mapPointOf(768.0, 512.0);   // the layer's own art
            Require(SharedFraction(floor, transform, left.first, left.second, 0) == 1.0,
                "copied ground must read as fully shared");
            Require(SharedFraction(floor, transform, right.first, right.second, 0) == 0.0,
                "the layer's own art must not read as shared");
            // A tile outside the one the floor covers says nothing, rather than claiming shared.
            const auto elsewhere = mapPointOf(256.0, 512.0 + 2048.0);
            Require(SharedFraction(floor, transform, elsewhere.first, elsewhere.second, 0) == 0.0,
                "a coordinate outside the covered tile must not report shared ground");

            // Only the floor next to the surface can share the surface's ground. 拉海's 星炬学院
            // floors are drawn from the surface drawing (51-84% of their pixels are the surface's
            // own), and their own markers stand on that art - but only the ground floor is at
            // ground level: 广场区 is the lowest floor and 运载区 the highest.
            auto floorAt = floor;
            floorAt.level = -1;
            Require(LayeredFloors::AdjacentToSurface(floorAt),
                "the floor next to the surface may share its ground");
            floorAt.level = -2;
            Require(!LayeredFloors::AdjacentToSurface(floorAt),
                "a floor above or below the surface must not use the comparison");
            floorAt.level = -4;
            Require(!LayeredFloors::AdjacentToSurface(floorAt),
                "so must a floor deeper in the building");

            // A layered map whose floors carry no number at all still gets an above/below order:
            // 星炬学院's zones are the plaza at the bottom, the teaching area, then the transport
            // area on top, against levels -1/-2/-3.
            {
                const auto zone = [](int layerId, const std::string& floorId, const std::string& name, int level) {
                    LayeredFloors::FloorEntry entry;
                    entry.layerId = layerId;
                    entry.floorId = floorId;
                    entry.floorName = name;
                    entry.level = level;
                    return entry;
                };
                std::vector<LayeredFloors::FloorEntry> academy{
                    zone(30, "-1/30", "星炬学院·广场区", -1),
                    zone(30, "-2/30", "星炬学院·教学区", -2),
                    zone(30, "-3/30", "星炬学院·运载区", -3),
                    zone(30, "-4/30", "文献中心·休憩区", -4),
                };
                Require(LayeredFloors::LayerHeightDirection(academy, 30) == -1,
                    "the academy's plaza is the lowest floor, so a bigger level is lower");
                // 下层金库 numbers its floors in the names, and they run the same way.
                std::vector<LayeredFloors::FloorEntry> vault{
                    zone(15, "-1/15", "贵金属与艺术品藏区1楼", -1),
                    zone(15, "-2/15", "贵金属与艺术品藏区2楼", -2),
                    zone(15, "-3/15", "贵金属与艺术品藏区3楼", -3),
                };
                Require(LayeredFloors::LayerHeightDirection(vault, 15) == -1,
                    "the vault's 1楼 is the ground floor");
                // 叩天关's names run the other way and are read from the same rule.
                std::vector<LayeredFloors::FloorEntry> pass{
                    zone(1, "-1/1", "叩天关·上层", -1),
                    zone(1, "-2/1", "叩天关·中层", -2),
                    zone(1, "-3/1", "叩天关·下层", -3),
                };
                Require(LayeredFloors::LayerHeightDirection(pass, 1) == 1,
                    "without a name that states an order, a bigger level is higher");
                // A name that states nothing is no evidence either way.
                std::vector<LayeredFloors::FloorEntry> unnamed{
                    zone(9, "-1/9", "愚人乐土", -1),
                    zone(9, "-2/9", "愚人乐土·深处", -2),
                };
                Require(LayeredFloors::LayerHeightDirection(unnamed, 9) == 1,
                    "two floors whose names state no order keep the common convention");

                // The ranks are what the markers actually use, and they survive a layer whose
                // heights do not follow its levels: 黯原's 虚妄摇篮 is 二层 (-3) at the bottom,
                // 入口 (-1) in the middle and 一层 (-2) on top.
                std::vector<LayeredFloors::FloorEntry> cradle{
                    zone(52, "-1/52", "入口·虚妄摇篮", -1),
                    zone(52, "-2/52", "一层·虚妄摇篮", -2),
                    zone(52, "-3/52", "二层·虚妄摇篮", -3),
                };
                LayeredFloors::AssignHeightRanks(cradle);
                Require(LayeredFloors::HeightRank(cradle, "-3/52") == 1 &&
                    LayeredFloors::HeightRank(cradle, "-1/52") == 2 &&
                    LayeredFloors::HeightRank(cradle, "-2/52") == 3,
                    "the cradle's ranks must be 二层 < 入口 < 一层");
                // 日树's floors are numbered upward, the opposite of the old default.
                std::vector<LayeredFloors::FloorEntry> tree{
                    zone(40, "-1/40", "一层·第一日树", -1),
                    zone(40, "-2/40", "二层·第一日树", -2),
                    zone(40, "-3/40", "三层·第一日树", -3),
                };
                LayeredFloors::AssignHeightRanks(tree);
                Require(LayeredFloors::HeightRank(tree, "-3/40") > LayeredFloors::HeightRank(tree, "-1/40"),
                    "the day tree's 三层 is above its 一层");
                // 秘藏之地's 地下一层 is the shallower of the two, so it ranks higher.
                std::vector<LayeredFloors::FloorEntry> hoard{
                    zone(18, "-1/18", "秘藏之地·地下一层", -1),
                    zone(18, "-2/18", "秘藏之地·地下二层", -2),
                };
                LayeredFloors::AssignHeightRanks(hoard);
                Require(LayeredFloors::HeightRank(hoard, "-1/18") > LayeredFloors::HeightRank(hoard, "-2/18"),
                    "地下一层 sits above 地下二层");

                // And that is what the markers do with it: standing on 入口, 一层 is above and
                // 二层 below - the ranks decide, and the level would have got both backwards.
                const auto rank = [&](const std::vector<LayeredFloors::FloorEntry>& floors, const char* id) {
                    return LayeredFloors::HeightRank(floors, id);
                };
                const int entrance = rank(cradle, "-1/52");
                Require(LayeredFloors::HeightComparison(entrance, -1, rank(cradle, "-2/52"), -2, 1) > 0,
                    "一层 must read as above 入口");
                Require(LayeredFloors::HeightComparison(entrance, -1, rank(cradle, "-3/52"), -3, 1) < 0,
                    "二层 must read as below 入口");
                // Without ranks the level comparison is what runs, unchanged.
                Require(LayeredFloors::HeightComparison(0, -1, 0, -2, 1) < 0,
                    "without ranks a bigger level is still higher");
                Require(LayeredFloors::HeightComparison(0, -1, 0, -2, -1) > 0,
                    "and the direction still flips it where the names say so");
            }
        }

        // Standing on ground the active floor copied from the surface: the floor's own markers
        // keep their roles and the surface's own markers come back, instead of one set being
        // hidden on a guess. This is 下层金库's plaza, where the surface marker three units away
        // belongs on screen and so does the marker just inside the vault door.
        {
            LayeredMap::SetForTest(SharedState(15, "-1/15", -1));
            Require(LayeredMap::RoleFor(Marker(8, "", "")) == MarkerRole::Normal,
                "a surface collectible must return on shared ground");
            Require(LayeredMap::RoleFor(Marker(8, "15", "0")) == MarkerRole::Normal,
                "a floor-less point must return on shared ground");
            Require(LayeredMap::RoleFor(Marker(8, "15", "-1000000/15")) == MarkerRole::Normal,
                "the entrance marker above the layer must return on shared ground");
            Require(LayeredMap::RoleFor(Marker(8, "15", "-1/15")) == MarkerRole::Current,
                "the floor's own marker stays current on shared ground");
            Require(LayeredMap::RoleFor(Marker(8, "15", "-2/15")) == MarkerRole::Above,
                "another floor of the same map keeps its above/below role on shared ground");
            Require(LayeredMap::RoleFor(Marker(8, "16", "-1/16")) == MarkerRole::Hidden,
                "a different layered map stays hidden on shared ground");
            // Off the copied piece the surface hides again.
            LayeredMap::SetForTest(State(15, "-1/15", -1));
            Require(LayeredMap::RoleFor(Marker(8, "", "")) == MarkerRole::Hidden,
                "surface markers must hide again off the shared ground");
        }

        std::cout << "Layered marker role tests passed\n";
        return 0;    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
