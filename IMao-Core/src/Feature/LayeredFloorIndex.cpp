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

bool Load(const std::filesystem::path& packDirectory, Index& index, std::string& error) {
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
                entry.tiles.push_back(std::move(parsedTile));
            }
        }
        std::string loadError;
        if (!FeatureBinaryCodec::Load(root / file, entry.features, loadError)) {
            error = "cannot load " + (root / file).string() + ": " + loadError;
            return false;
        }
        if (entry.features.imgKeypoints.empty() ||
            entry.features.imgDescriptors.rows != static_cast<int>(entry.features.imgKeypoints.size())) {
            error = "layered floor " + entry.floorId + " has an invalid feature set";
            return false;
        }
        // The footprint centre, so a cold start has somewhere to point its search.
        double sumX = 0.0, sumY = 0.0;
        std::size_t cells = 0;
        const auto bitsPerTile = static_cast<std::size_t>(index.transform.gridSize) * index.transform.gridSize;
        for (const auto& tile : entry.tiles) {
            if (tile.occupancy.size() * 4 != bitsPerTile) continue;
            for (std::size_t bit = 0; bit < bitsPerTile; ++bit) {
                const char nibble = tile.occupancy[bit / 4];
                const int value = nibble >= '0' && nibble <= '9' ? nibble - '0'
                    : (nibble >= 'a' && nibble <= 'f' ? nibble - 'a' + 10
                        : (nibble >= 'A' && nibble <= 'F' ? nibble - 'A' + 10 : 0));
                if (((value >> (bit % 4)) & 1) == 0) continue;
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
        }
        index.floors.push_back(std::move(entry));
    }
    if (index.floors.empty()) {
        error = "layered floor index lists no floors";
        return false;
    }
    return true;
}

bool Contains(const FloorEntry& floor, const Transform& transform, double mapX, double mapY) {    // Inverse of the builder's KuroTilePointToAppMap: map -> game -> tile pixel -> grid cell.
    const double gameX = (mapX - transform.originX) / transform.scale;
    const double gameY = (mapY - transform.originY) / transform.scale;
    const int tileX = static_cast<int>(std::floor(gameX / transform.virtualMapSize + 1.0));
    const int tileY = static_cast<int>(std::ceil(-gameY / transform.virtualMapSize));
    const double pixelX = gameX * transform.tileSize / transform.virtualMapSize + transform.tileSize -
        static_cast<double>(tileX) * transform.tileSize;
    const double pixelY = static_cast<double>(tileY) * transform.tileSize +
        gameY * transform.tileSize / transform.virtualMapSize;
    const double cell = transform.tileSize / transform.gridSize;
    if (pixelX < 0.0 || pixelY < 0.0 || pixelX >= transform.tileSize || pixelY >= transform.tileSize) return false;
    const int cellX = static_cast<int>(pixelX / cell);
    const int cellY = static_cast<int>(pixelY / cell);
    for (const auto& tile : floor.tiles) {
        if (tile.x != tileX || tile.y != tileY) continue;
        const auto bits = static_cast<std::size_t>(transform.gridSize) * transform.gridSize;
        if (tile.occupancy.size() * 4 != bits) return false;
        // One cell of slack: the player's position is accurate to a couple of map pixels and
        // the cave edge is exactly where a strict test would flicker.
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const int gx = cellX + dx;
                const int gy = cellY + dy;
                if (gx < 0 || gy < 0 || gx >= transform.gridSize || gy >= transform.gridSize) continue;
                const auto bit = static_cast<std::size_t>(gy) * transform.gridSize + gx;
                const char nibble = tile.occupancy[bit / 4];
                const int value = nibble >= '0' && nibble <= '9' ? nibble - '0'
                    : (nibble >= 'a' && nibble <= 'f' ? nibble - 'a' + 10
                        : (nibble >= 'A' && nibble <= 'F' ? nibble - 'A' + 10 : 0));
                // The writer packs four cells per nibble, first cell in the lowest bit.
                if ((value >> (bit % 4)) & 1) return true;
            }
        }
        return false;
    }
    return false;
}

Classification Classify(const ImageFeatureData& query, const std::vector<FloorEntry>& floors,
    int minimumMatches, double margin, float ratio, float maxDistance) {
    Classification result;
    if (query.imgDescriptors.empty() || query.imgDescriptors.rows < 2 || floors.empty()) return result;

    cv::BFMatcher matcher(cv::NORM_L2);
    for (const auto& floor : floors) {
        if (floor.features.imgDescriptors.empty()) continue;
        std::vector<std::vector<cv::DMatch>> knn;
        matcher.knnMatch(query.imgDescriptors, floor.features.imgDescriptors, knn, 2);
        FloorVote vote;
        vote.layerId = floor.layerId;
        vote.floorId = floor.floorId;
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
