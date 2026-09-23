#include "LayeredFloorIndex.h"

#include "Processing/FeatureBinaryCodec.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>

using json = nlohmann::json;

namespace LayeredFloors {
namespace {

std::string ReadString(const json& node, const char* key) {
    const auto it = node.find(key);
    return it == node.end() || it->is_null() ? std::string() : it->get<std::string>();
}

} // namespace

int FloorLevel(const std::string& floorId) {
    const auto slash = floorId.find('/');
    if (slash == std::string::npos || slash == 0) return 0;
    try {
        return std::stoi(floorId.substr(0, slash));
    }
    catch (...) {
        return 0;
    }
}

int FloorLayerId(const std::string& floorId) {
    const auto slash = floorId.find('/');
    if (slash == std::string::npos || slash + 1 >= floorId.size()) return 0;
    try {
        return std::stoi(floorId.substr(slash + 1));
    }
    catch (...) {
        return 0;
    }
}

/// Centre of one occupancy cell in map coordinates - the inverse of the mapping Contains uses.
void MapPointOfCell(const Transform& transform, int tileX, int tileY, int cellX, int cellY,
    double& mapX, double& mapY) {
    const double cell = transform.tileSize / transform.gridSize;
    const double pixelX = (cellX + 0.5) * cell;
    const double pixelY = (cellY + 0.5) * cell;
    const double kuroX = static_cast<double>(tileX) * transform.tileSize + pixelX;
    const double kuroY = static_cast<double>(tileY) * transform.tileSize - pixelY;
    const double gameX = (kuroX - transform.tileSize) * transform.virtualMapSize / transform.tileSize;
    const double gameY = -kuroY * transform.virtualMapSize / transform.tileSize;
    mapX = gameX * transform.scale + transform.originX;
    mapY = gameY * transform.scale + transform.originY;
}

bool Load(const std::filesystem::path& packDirectory, Index& index, std::string& error,
    int maxKeypointsPerFloor) {
    index.floors.clear();
    const auto indexPath = packDirectory / "layered-floors" / "floor-index.json";
    if (!std::filesystem::exists(indexPath)) {
        error = "no layered floor index at " + indexPath.string();
        return false;
    }
    std::ifstream input(indexPath);
    if (!input) {
        error = "cannot open " + indexPath.string();
        return false;
    }
    json parsed;
    try {
        input >> parsed;
    }
    catch (const std::exception& exception) {
        error = "cannot parse " + indexPath.string() + ": " + exception.what();
        return false;
    }
    if (parsed.value("formatVersion", 0) != 1) {
        error = "unsupported layered floor index format";
        return false;
    }
    const auto entries = parsed.find("floors");
    if (entries == parsed.end() || !entries->is_array()) {
        error = "layered floor index has no floors array";
        return false;
    }
    const auto transformNode = parsed.find("coordinateTransform");
    if (transformNode != parsed.end() && transformNode->is_object()) {
        index.transform.originX = transformNode->value("originX", index.transform.originX);
        index.transform.originY = transformNode->value("originY", index.transform.originY);
        index.transform.scale = transformNode->value("scale", index.transform.scale);
        index.transform.virtualMapSize = transformNode->value("virtualMapSize", index.transform.virtualMapSize);
        index.transform.tileSize = transformNode->value("tileSize", index.transform.tileSize);
    }
    index.transform.gridSize = parsed.value("gridSize", index.transform.gridSize);
    if (index.transform.gridSize <= 0 || index.transform.scale <= 0.0) {
        error = "layered floor index has an invalid transform";
        return false;
    }
    const auto root = indexPath.parent_path();
    for (const auto& node : *entries) {
        FloorEntry entry;
        entry.layerId = node.value("layerId", 0);
        entry.floorId = ReadString(node, "floorId");
        entry.layerName = ReadString(node, "layerName");
        entry.floorName = ReadString(node, "floorName");
        entry.level = FloorLevel(entry.floorId);
        const auto file = ReadString(node, "file");
        if (entry.floorId.empty() || file.empty()) {
            error = "layered floor index entry is missing floorId or file";
            return false;
        }
        const auto tiles = node.find("tiles");
        if (tiles != node.end() && tiles->is_array()) {
            for (const auto& tile : *tiles) {
                FloorTile parsedTile;
                parsedTile.x = tile.value("x", 0);
                parsedTile.y = tile.value("y", 0);
                parsedTile.occupancy = ReadString(tile, "occupancy");
                parsedTile.shared = ReadString(tile, "shared");
                entry.tiles.push_back(std::move(parsedTile));
            }
        }
        std::string loadError;
        if (!FeatureBinaryCodec::Load(root / file, entry.features, loadError)) {
            error = "cannot load " + (root / file).string() + ": " + loadError;
            return false;
        }
        // The index answers "which floor", never "where exactly" - the pack does that. Keeping a
        // The full set stays: it is what the deciding vote uses. The cap only fills the sample the
        // cold start compares against every floor in the game - see sampleFeatures' comment for the
        // measurement that made this split necessary (a 600-descriptor sample picked the wrong
        // floor in 下层金库 where the full set picked the right one).
        entry.sampleFeatures = entry.features;
        if (maxKeypointsPerFloor > 0 &&
            entry.features.imgKeypoints.size() > static_cast<std::size_t>(maxKeypointsPerFloor)) {
            const std::size_t stride = (entry.features.imgKeypoints.size() +
                static_cast<std::size_t>(maxKeypointsPerFloor) - 1) / static_cast<std::size_t>(maxKeypointsPerFloor);
            std::vector<cv::KeyPoint> keypoints;
            std::vector<float> descriptors;
            keypoints.reserve(entry.features.imgKeypoints.size() / stride + 1);
            descriptors.reserve(keypoints.capacity() * 128);
            for (std::size_t i = 0; i < entry.features.imgKeypoints.size(); i += stride) {
                keypoints.push_back(entry.features.imgKeypoints[i]);
                const float* row = entry.features.imgDescriptors.ptr<float>(static_cast<int>(i));
                descriptors.insert(descriptors.end(), row, row + entry.features.imgDescriptors.cols);
            }
            entry.sampleFeatures.imgKeypoints = std::move(keypoints);
            entry.sampleFeatures.imgDescriptors = cv::Mat(
                static_cast<int>(entry.sampleFeatures.imgKeypoints.size()),
                entry.features.imgDescriptors.cols, CV_32F, descriptors.data()).clone();
        }
        if (entry.features.imgKeypoints.empty() ||
            entry.features.imgDescriptors.rows != static_cast<int>(entry.features.imgKeypoints.size())) {
            error = "layered floor " + entry.floorId + " has an invalid feature set";
            return false;
        }
        // The footprint centre, so a cold start has somewhere to point its search. The same pass
        // counts how much of the floor's art is the surface's (see FloorEntry::copiedFraction).
        double sumX = 0.0, sumY = 0.0;
        std::size_t cells = 0, copiedCells = 0;
        const auto bitsPerTile = static_cast<std::size_t>(index.transform.gridSize) * index.transform.gridSize;
        const auto nibbleValue = [](char nibble) {
            return nibble >= '0' && nibble <= '9' ? nibble - '0'
                : (nibble >= 'a' && nibble <= 'f' ? nibble - 'a' + 10
                    : (nibble >= 'A' && nibble <= 'F' ? nibble - 'A' + 10 : 0));
        };
        for (const auto& tile : entry.tiles) {
            if (tile.occupancy.size() * 4 != bitsPerTile) continue;
            const bool hasShared = tile.shared.size() * 4 == bitsPerTile;
            for (std::size_t bit = 0; bit < bitsPerTile; ++bit) {
                const int value = nibbleValue(tile.occupancy[bit / 4]);
                if (((value >> (bit % 4)) & 1) == 0) continue;
                if (hasShared && (((nibbleValue(tile.shared[bit / 4]) >> (bit % 4)) & 1) != 0)) ++copiedCells;
                double cellX = 0.0, cellY = 0.0;
                MapPointOfCell(index.transform, tile.x, tile.y,
                    static_cast<int>(bit % index.transform.gridSize),
                    static_cast<int>(bit / index.transform.gridSize), cellX, cellY);
                sumX += cellX; sumY += cellY; ++cells;
            }
        }
        if (cells > 0) {
            entry.centerMapX = sumX / static_cast<double>(cells);
            entry.centerMapY = sumY / static_cast<double>(cells);
            entry.hasCenter = true;
            entry.copiedFraction = static_cast<double>(copiedCells) / static_cast<double>(cells);
        }
        index.floors.push_back(std::move(entry));
    }
    if (index.floors.empty()) {
        error = "layered floor index lists no floors";
        return false;
    }
    // Which way is up, and where each floor sits. Upstream names floors several ways and the level
    // runs opposite ways in them: 叩天关/眠龙庭/雾隐阁 use 上层(-1) ... 下层(-3), where a bigger
    // level is higher, while 下层金库's 贵金属与艺术品藏区 uses 1楼(-1) ... 4楼(-4), where 4楼 is
    // the top. Reading the second kind with the first convention marked the floors above the
    // player as below.
    AssignHeightRanks(index.floors);
    return true;
}

namespace {

/// Floor names whose order is a fact about the game rather than something the name says, recorded
/// from play (2026-09-23) and checked against the game by the user:
///
///   星炬学院 is an above-ground building and its zones run 广场区 (ground) < 教学区 < 运载区 (top).
///   虚妄摇篮's heights are NOT monotonic in its levels: 二层 (-3) is the bottom, 入口 (-1) is in
///   the middle and 一层 (-2) is the top. A single "which way do the levels run" cannot express
///   that, which is why every floor of a layer that appears here is listed.
///
/// A layered map shipped later with another naming of this kind needs one line here and nothing
/// else; every other branch of the rule reads the position out of the name.
struct RecordedOrder { const char* name; int order; };   // ascending with height
constexpr RecordedOrder kRecordedOrders[] = {
    { "广场区", 1 }, { "教学区", 2 }, { "运载区", 3 },
    { "二层·虚妄摇篮", 1 }, { "入口·虚妄摇篮", 2 }, { "一层·虚妄摇篮", 3 },
};

/// One Chinese digit, or 0 for anything else. The names are UTF-8, so the digit is the three bytes
/// before the 层 that follows it - indexing a single byte there would read half a character.
int ChineseDigitBefore(const std::string& name, std::size_t end) {
    static const char* const digits[] = { "一", "二", "三", "四", "五", "六", "七", "八", "九", "十" };
    if (end < 3) return 0;
    const std::string character = name.substr(end - 3, 3);
    for (int index = 0; index < 10; ++index) {
        if (character == digits[index]) return index + 1;
    }
    return 0;
}

/// The vertical position a floor name states, ascending with height, or 0 when it states none.
///
/// Upstream's `sort` field is no help: it simply repeats the level order (叩天关's 上层 and
/// 下层金库's 1楼 both come first, and those are physically opposite), and no coordinate in the
/// data carries a height. So the name is the source, read in this order:
///
///   "…4楼"        the building floor, numbered from the ground up in this game;
///   "…二层"        the same, written in Chinese; "地下一层" is below ground, so it counts down;
///   recorded      names that state no position at all - see kRecordedOrders.
///
/// The last case is why 一层 cannot simply be read as "the first floor up": it is the ground floor
/// in 拉海's 日树 but the TOP floor of 黯原's 虚妄摇篮, and the recorded table is what separates
/// them. Reading it as "up" without that table would swap the markers on the other map.
int NamedFloorOrder(const std::string& name) {
    for (const auto& recorded : kRecordedOrders) {
        if (name.find(recorded.name) != std::string::npos) return recorded.order;
    }
    const auto marker = name.find("楼");
    if (marker != std::string::npos) {
        std::size_t begin = marker;
        while (begin > 0 && name[begin - 1] >= '0' && name[begin - 1] <= '9') --begin;
        if (begin != marker) return std::stoi(name.substr(begin, marker - begin));
    }
    const auto storey = name.find("层");
    if (storey != std::string::npos) {
        const int digit = ChineseDigitBefore(name, storey);
        if (digit > 0) {
            // "地下一层" counts down from the surface, so 地下一层 sits above 地下二层.
            const bool belowGround = storey >= 9 && name.compare(storey - 6, 3, "下") == 0 &&
                name.compare(storey - 9, 3, "地") == 0;
            return belowGround ? -digit : digit;
        }
    }
    return 0;
}

} // namespace

int LayerHeightDirection(const std::vector<FloorEntry>& floors, int layerId) {
    std::vector<std::pair<int, int>> numbered;   // (floor position, level)
    for (const auto& floor : floors) {
        if (floor.layerId != layerId) continue;
        const int order = NamedFloorOrder(floor.floorName);
        // 0 means the name states no position; 地下一层's negative order is a position like any other.
        if (order != 0) numbered.emplace_back(order, floor.level);
    }
    // Fewer than two floors have a name that states a position, so there is nothing to compare and
    // the common convention is kept: a bigger level is higher.
    if (numbered.size() < 2) return 1;
    int agreeing = 0, disagreeing = 0;
    for (std::size_t i = 0; i < numbered.size(); ++i) {
        for (std::size_t j = i + 1; j < numbered.size(); ++j) {
            const bool numberRises = numbered[i].first > numbered[j].first;
            const bool levelRises = numbered[i].second > numbered[j].second;
            if (numberRises == levelRises) ++agreeing; else ++disagreeing;
        }
    }
    return disagreeing > agreeing ? -1 : 1;
}

bool AdjacentToSurface(const FloorEntry& floor) {
    // Upstream numbers the floor next to the surface -1 on every layered map: the entrance floor
    // of a cave (叩天关·上层, 幽锁层·三层), the ground floor of a building (下层金库's 1楼, and
    // 星炬学院·广场区, which is 星炬学院's lowest floor). Every other floor is above or below the
    // surface, so art it copied from the surface drawing is not surface ground.
    return floor.level == -1;
}

int HeightRank(const std::vector<FloorEntry>& floors, const std::string& floorId) {
    for (const auto& floor : floors) {
        if (floor.floorId == floorId) return floor.heightRank;
    }
    return 0;
}

int HeightComparison(int currentRank, int currentLevel, int markerRank, int markerLevel, int direction) {
    if (currentRank != 0 && markerRank != 0 && markerRank != currentRank) {
        return markerRank < currentRank ? -1 : 1;
    }
    const int sign = direction == 0 ? 1 : direction;
    // Equal ranks mean the two floors are the same place as far as the names say; the level then
    // decides, exactly as it did before any name was read.
    return markerLevel * sign < currentLevel * sign ? -1 : 1;
}

void AssignHeightRanks(std::vector<FloorEntry>& floors) {
    std::vector<int> layers;
    for (const auto& floor : floors) {
        if (std::find(layers.begin(), layers.end(), floor.layerId) == layers.end()) layers.push_back(floor.layerId);
    }
    for (const int layerId : layers) {
        const int direction = LayerHeightDirection(floors, layerId);
        // Where every floor of the layer states its own position, those positions are used as they
        // are: 虚妄摇篮's heights are not monotonic in its levels, so no single direction can
        // reproduce them (入口 sits between 二层 and 一层).
        std::size_t total = 0, named = 0;
        for (const auto& floor : floors) {
            if (floor.layerId != layerId) continue;
            ++total;
            if (NamedFloorOrder(floor.floorName) != 0) ++named;
        }
        const bool everyFloorNamed = total > 1 && named == total;
        for (auto& floor : floors) {
            if (floor.layerId != layerId) continue;
            floor.heightDirection = direction;
            const int order = NamedFloorOrder(floor.floorName);
            floor.heightRank = everyFloorNamed && order != 0 ? order : floor.level * direction;
        }
    }
}

namespace {

/// One grid bit, packed four cells per nibble with the first cell in the lowest bit.
int GridBit(const std::string& grid, std::size_t bit) {
    const char nibble = grid[bit / 4];
    const int value = nibble >= '0' && nibble <= '9' ? nibble - '0'
        : (nibble >= 'a' && nibble <= 'f' ? nibble - 'a' + 10
            : (nibble >= 'A' && nibble <= 'F' ? nibble - 'A' + 10 : 0));
    return (value >> (bit % 4)) & 1;
}

struct CellLocation {
    const FloorTile* tile = nullptr;
    int cellX = 0;
    int cellY = 0;
};

/// The floor's tile holding this coordinate and the grid cell it falls in. Inverse of the builder's
/// KuroTilePointToAppMap: map -> game -> tile pixel -> grid cell. A tile the floor does not cover -
/// or one with an unusable grid - is no tile at all.
CellLocation LocateCell(const FloorEntry& floor, const Transform& transform, double mapX, double mapY) {
    CellLocation location;
    const double gameX = (mapX - transform.originX) / transform.scale;
    const double gameY = (mapY - transform.originY) / transform.scale;
    const int tileX = static_cast<int>(std::floor(gameX / transform.virtualMapSize + 1.0));
    const int tileY = static_cast<int>(std::ceil(-gameY / transform.virtualMapSize));
    const double pixelX = gameX * transform.tileSize / transform.virtualMapSize + transform.tileSize -
        static_cast<double>(tileX) * transform.tileSize;
    const double pixelY = static_cast<double>(tileY) * transform.tileSize +
        gameY * transform.tileSize / transform.virtualMapSize;
    const double cell = transform.tileSize / transform.gridSize;
    if (pixelX < 0.0 || pixelY < 0.0 || pixelX >= transform.tileSize || pixelY >= transform.tileSize) {
        return location;
    }
    location.cellX = static_cast<int>(pixelX / cell);
    location.cellY = static_cast<int>(pixelY / cell);
    const auto bits = static_cast<std::size_t>(transform.gridSize) * transform.gridSize;
    for (const auto& tile : floor.tiles) {
        if (tile.x != tileX || tile.y != tileY) continue;
        if (tile.occupancy.size() * 4 != bits) return CellLocation{};
        location.tile = &tile;
        return location;
    }
    return location;
}

} // namespace

bool Contains(const FloorEntry& floor, const Transform& transform, double mapX, double mapY) {
    const auto location = LocateCell(floor, transform, mapX, mapY);
    if (location.tile == nullptr) return false;
    // One cell of slack: the player's position is accurate to a couple of map pixels and the cave
    // edge is exactly where a strict test would flicker.
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int gx = location.cellX + dx;
            const int gy = location.cellY + dy;
            if (gx < 0 || gy < 0 || gx >= transform.gridSize || gy >= transform.gridSize) continue;
            if (GridBit(location.tile->occupancy, static_cast<std::size_t>(gy) * transform.gridSize + gx)) return true;
        }
    }
    return false;
}

bool InsideWithMargin(const FloorEntry& floor, const Transform& transform, double mapX, double mapY,
    int marginCells) {
    const auto location = LocateCell(floor, transform, mapX, mapY);
    if (location.tile == nullptr) return false;
    // Every cell within the margin has to be the floor's art. NOTE: the layer art is a drawing -
    // the rooms inside 虚妄摇篮's 一层 are transparent holes in it - so this says "standing on drawn
    // art", NOT "inside the cave". It is not a usable "am I inside" test; kept only because the
    // grid it shares with Contains is useful for diagnosing a footprint.
    for (int dy = -marginCells; dy <= marginCells; ++dy) {
        for (int dx = -marginCells; dx <= marginCells; ++dx) {
            const int gx = location.cellX + dx;
            const int gy = location.cellY + dy;
            if (gx < 0 || gy < 0 || gx >= transform.gridSize || gy >= transform.gridSize) return false;
            if (!GridBit(location.tile->occupancy, static_cast<std::size_t>(gy) * transform.gridSize + gx)) return false;
        }
    }
    return true;
}

double SharedFraction(const FloorEntry& floor, const Transform& transform, double mapX, double mapY,
    int radiusCells) {    const double gameX = (mapX - transform.originX) / transform.scale;
    const double gameY = (mapY - transform.originY) / transform.scale;
    const int tileX = static_cast<int>(std::floor(gameX / transform.virtualMapSize + 1.0));
    const int tileY = static_cast<int>(std::ceil(-gameY / transform.virtualMapSize));
    const double pixelX = gameX * transform.tileSize / transform.virtualMapSize + transform.tileSize -
        static_cast<double>(tileX) * transform.tileSize;
    const double pixelY = static_cast<double>(tileY) * transform.tileSize +
        gameY * transform.tileSize / transform.virtualMapSize;
    const double cell = transform.tileSize / transform.gridSize;
    if (pixelX < 0.0 || pixelY < 0.0 || pixelX >= transform.tileSize || pixelY >= transform.tileSize) return 0.0;
    const int cellX = static_cast<int>(pixelX / cell);
    const int cellY = static_cast<int>(pixelY / cell);
    const auto bits = static_cast<std::size_t>(transform.gridSize) * transform.gridSize;
    for (const auto& tile : floor.tiles) {
        if (tile.x != tileX || tile.y != tileY) continue;
        // An index built before the shared grid existed simply has none: nothing is shared.
        if (tile.shared.size() * 4 != bits || tile.occupancy.size() * 4 != bits) return 0.0;
        int opaque = 0, shared = 0;
        for (int dy = -radiusCells; dy <= radiusCells; ++dy) {
            for (int dx = -radiusCells; dx <= radiusCells; ++dx) {
                const int gx = cellX + dx;
                const int gy = cellY + dy;
                if (gx < 0 || gy < 0 || gx >= transform.gridSize || gy >= transform.gridSize) continue;
                const auto bit = static_cast<std::size_t>(gy) * transform.gridSize + gx;
                if (GridBit(tile.occupancy, bit) == 0) continue;   // outside the art is not evidence
                ++opaque;
                shared += GridBit(tile.shared, bit);
            }
        }
        return opaque > 0 ? static_cast<double>(shared) / opaque : 0.0;
    }
    return 0.0;
}

Classification Classify(const ImageFeatureData& query, const std::vector<const FloorEntry*>& floors,
    int minimumMatches, double margin, float ratio, float maxDistance, bool useSamples) {
    Classification result;
    if (query.imgDescriptors.empty() || query.imgDescriptors.rows < 2 || floors.empty()) return result;

    cv::BFMatcher matcher(cv::NORM_L2);
    for (std::size_t index = 0; index < floors.size(); ++index) {
        const auto* floor = floors[index];
        if (floor == nullptr) continue;
        // The cold start compares every floor in the game and can afford the capped sample; the
        // vote that decides a floor compares a handful and uses the full set.
        const auto& source = useSamples && !floor->sampleFeatures.imgDescriptors.empty()
            ? floor->sampleFeatures : floor->features;
        if (source.imgDescriptors.empty()) continue;
        std::vector<std::vector<cv::DMatch>> knn;
        matcher.knnMatch(query.imgDescriptors, source.imgDescriptors, knn, 2);
        FloorVote vote;
        vote.layerId = floor->layerId;
        vote.floorId = floor->floorId;
        vote.sourceIndex = index;
        for (const auto& pair : knn) {
            if (pair.size() < 2) continue;
            // Same test the localizer uses: nearest neighbour must beat the second by a ratio
            // and stay inside an absolute distance.
            if (pair[0].distance < ratio * pair[1].distance && pair[0].distance < maxDistance) ++vote.matches;
        }
        result.votes.push_back(std::move(vote));
    }
    std::sort(result.votes.begin(), result.votes.end(),
        [](const FloorVote& left, const FloorVote& right) {
            if (left.matches != right.matches) return left.matches > right.matches;
            return left.floorId < right.floorId;
        });
    if (result.votes.empty()) return result;

    result.winnerMatches = result.votes.front().matches;
    result.runnerUpMatches = result.votes.size() > 1 ? result.votes[1].matches : 0;
    result.layerId = result.votes.front().layerId;
    result.floorId = result.votes.front().floorId;
    const bool enough = result.winnerMatches >= minimumMatches;
    const bool leads = result.runnerUpMatches == 0 ||
        static_cast<double>(result.winnerMatches) >= margin * static_cast<double>(result.runnerUpMatches);
    result.identified = enough && leads;
    return result;
}

} // namespace LayeredFloors

