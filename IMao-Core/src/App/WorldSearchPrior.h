#pragma once

#include "../Coordinate/CoordinateStruct.h"
#include "../Feature/RuntimeFeatureRepository.h"

#include <filesystem>
#include <string>
#include <vector>

// A scene-specific search hint, never a location result. The historical name
// is retained for callers; sceneId now isolates every independent map space.
// Country labels are advisory and available only for the World hierarchy.
struct WorldSearchPrior {
    bool valid = false;
    int sceneId = 0;
    Coordinate centerMapCoordinate;
    double radius = 0.0;
    std::size_t candidateTileCount = 0;
    std::string areaName;
};

class WorldSearchPriorIndex {
public:
    bool Load(const std::filesystem::path& countryPath, std::string& error);

    WorldSearchPrior Build(const RuntimeFeatureResources& resources,
        const Coordinate& centerMapCoordinate, double radius, int sceneId = 1) const;

private:
    struct AreaAnchor {
        Coordinate mapCoordinate;
        std::string name;
    };

    std::vector<AreaAnchor> anchors_;
};
