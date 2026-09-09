#include "WorldSearchPrior.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>

namespace {
constexpr int kWorldStateId = 8;

bool IntersectsRadius(const MapVisualTile& tile, const Coordinate& center, double radius) {
    const double nearestX = std::clamp(center.x, static_cast<double>(tile.minX), static_cast<double>(tile.maxX));
    const double nearestY = std::clamp(center.y, static_cast<double>(tile.minY), static_cast<double>(tile.maxY));
    return std::hypot(center.x - nearestX, center.y - nearestY) <= radius;
}
}

bool WorldSearchPriorIndex::Load(const std::filesystem::path& countryPath, std::string& error) {
    anchors_.clear();
    error.clear();
    try {
        std::ifstream input(countryPath);
        if (!input) {
            error = "country hierarchy is unavailable";
            return false;
        }
        const nlohmann::json countries = nlohmann::json::parse(input);
        if (!countries.is_array()) {
            error = "country hierarchy root is invalid";
            return false;
        }

        for (const auto& country : countries) {
            const std::string countryName = country.value("name", std::string());
            const auto entries = country.find("countrys");
            if (entries == country.end() || !entries->is_array()) continue;
            for (const auto& entry : *entries) {
                if (entry.value("stateId", 0) != kWorldStateId) continue;
                const double rawX = entry.value("xPosition", std::numeric_limits<double>::quiet_NaN());
                const double rawY = entry.value("yPosition", std::numeric_limits<double>::quiet_NaN());
                if (!std::isfinite(rawX) || !std::isfinite(rawY)) continue;

                // Kuro's country hierarchy uses centi-units for state 8.  It
                // has no region polygons, therefore these points only name a
                // nearby area and never constrain the matcher.
                AreaAnchor anchor;
                const auto* world = Scene::Find(Scene::SceneNameToId("World"));
                if (world == nullptr) continue;
                anchor.mapCoordinate = Coordinate(rawX / 100.0 * world->scale + world->originX,
                    rawY / 100.0 * world->scale + world->originY);
                const std::string entryName = entry.value("name", std::string());
                anchor.name = countryName;
                if (!entryName.empty()) {
                    if (!anchor.name.empty()) anchor.name += "/";
                    anchor.name += entryName;
                }
        // Coordinate::IsValid() is intentionally non-const in the legacy
        // coordinate type.  Country anchors are only advisory labels, so an
        // all-zero transformed point is sufficient to reject malformed rows
        // without mutating the parsed anchor.
        if ((anchor.mapCoordinate.x != 0.0 || anchor.mapCoordinate.y != 0.0) && !anchor.name.empty()) {
            anchors_.push_back(std::move(anchor));
        }
            }
        }
        return true;
    }
    catch (const std::exception& exception) {
        anchors_.clear();
        error = exception.what();
        return false;
    }
}

WorldSearchPrior WorldSearchPriorIndex::Build(const RuntimeFeatureResources& resources,
    const Coordinate& centerMapCoordinate, double radius, int sceneId) const {
    WorldSearchPrior prior;
    if (radius <= 0.0 || !std::isfinite(radius) || !std::isfinite(centerMapCoordinate.x) ||
        !std::isfinite(centerMapCoordinate.y) || !Scene::IsRuntimeApproved(sceneId) || !resources.visualIndexReady) return prior;

    prior.valid = true;
    prior.sceneId = sceneId;
    prior.centerMapCoordinate = centerMapCoordinate;
    prior.radius = radius;
    for (std::size_t index = 0; index < resources.visualIndex.tiles.size(); ++index) {
        const int tileScene = index < resources.baseVisualTileCount ? 1 : resources.visualIndex.tiles[index].sceneId;
        if (tileScene != sceneId) continue;
        if (IntersectsRadius(resources.visualIndex.tiles[index], centerMapCoordinate, radius)) {
            ++prior.candidateTileCount;
        }
    }

    if (sceneId != Scene::SceneNameToId("World")) {
        prior.areaName = Scene::Find(sceneId)->name;
        return prior;
    }
    double nearestDistance = std::numeric_limits<double>::infinity();
    for (const auto& anchor : anchors_) {
        const double distance = std::hypot(anchor.mapCoordinate.x - centerMapCoordinate.x,
            anchor.mapCoordinate.y - centerMapCoordinate.y);
        if (distance < nearestDistance) {
            nearestDistance = distance;
            prior.areaName = anchor.name;
        }
    }
    return prior;
}
