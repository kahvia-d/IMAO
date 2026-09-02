#pragma once

#include "../Coordinate/CoordinateStruct.h"
#include "../Feature/RuntimeFeatureRepository.h"

#include <filesystem>
#include <string>
#include <vector>

// A WorldSearchPrior is deliberately a search hint, never a location result.
// State 8 shares one coordinate system, so the accepted minimap coordinate is
// more reliable than a semantic region name.  The name is only useful in
// diagnostics when an operator wants to understand which part of World was
// selected.
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
        const Coordinate& centerMapCoordinate, double radius) const;

private:
    struct AreaAnchor {
        Coordinate mapCoordinate;
        std::string name;
    };

    std::vector<AreaAnchor> anchors_;
};
