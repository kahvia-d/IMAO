#pragma once

#include "../Coordinate/CoordinateStruct.h"
#include "../Feature/RuntimeFeatureRepository.h"
#include "WorldSearchPrior.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

enum class MapViewportSearchScope {
    Local512,
    Local1024,
    Global
};

struct MapViewportLocalizationRequest {
    // App instances can be stopped and started without unloading the DLL.
    // Keep their queued viewport work isolated so a late result from a former
    // run can never be confused with the same generation number in a new run.
    std::uint64_t sessionId = 0;
    std::uint64_t uiGeneration = 0;
    std::uint64_t viewportGeneration = 0;
    std::uint64_t frameId = 0;
    cv::Mat mapCrop;
    MapViewportSearchScope scope = MapViewportSearchScope::Global;
    std::optional<WorldSearchPrior> prior;
};

struct MapViewportLocalizationResult {
    std::uint64_t sessionId = 0;
    std::uint64_t uiGeneration = 0;
    std::uint64_t viewportGeneration = 0;
    std::uint64_t frameId = 0;
    MapViewportSearchScope scope = MapViewportSearchScope::Global;
    bool accepted = false;
    Coordinate centerMapCoordinate;
    std::vector<cv::Point2f> captureCorners;
    int goodMatchCount = 0;
    int cropKeypointCount = 0;
    double durationMilliseconds = 0.0;
};

// Dedicated worker for the full-screen map.  It intentionally does not share
// GlobalVisualLocalizer's worker: a slow full-map viewport search must never
// delay minimap recovery or the main capture loop.
class MapViewportLocalizer {
public:
    static bool Initialize(std::shared_ptr<const RuntimeFeatureResources> resources, std::string& error);
    static void Shutdown();
    static bool IsReady();
    static bool Submit(MapViewportLocalizationRequest request);
    static bool TryTakeLatestResult(MapViewportLocalizationResult& result);
    static const char* ScopeName(MapViewportSearchScope scope);
};
