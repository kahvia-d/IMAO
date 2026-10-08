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
    std::uint64_t requestId = 0;
    std::uint64_t viewportRevision = 0;
    cv::Mat mapCrop;
    MapViewportSearchScope scope = MapViewportSearchScope::Global;
    std::optional<WorldSearchPrior> prior;
};

struct MapViewportLocalizationResult {
    std::uint64_t sessionId = 0;
    std::uint64_t uiGeneration = 0;
    std::uint64_t viewportGeneration = 0;
    std::uint64_t frameId = 0;
    std::uint64_t requestId = 0;
    std::uint64_t viewportRevision = 0;
    MapViewportSearchScope scope = MapViewportSearchScope::Global;
    bool accepted = false;
    int sceneId = 0;
    Coordinate centerMapCoordinate;
    std::vector<cv::Point2f> captureCorners;
    int goodMatchCount = 0;
    int inlierCount = 0;
    double inlierRatio = 0.0;
    double medianReprojectionError = 0.0;
    int coveredQuadrants = 0;
    int cropKeypointCount = 0;
    double durationMilliseconds = 0.0;
    // Cold start only. Without a prior this search used to compare the viewport against every
    // tile of every approved scene; now the visual index ranks those tiles first and the ranking
    // may narrow what is compared. These say what it contributed, so the thresholds in
    // MapViewportLocalizer can be calibrated from a field session instead of from an estimate.
    // A zero rankedSceneCount means the index named nothing and the search ran the sweep, exactly
    // as it always did.
    int retrievalRankedSceneCount = 0;
    int retrievalSceneCount = 0;
    int retrievalSceneId = 0;
    double retrievalTopScore = 0.0;
    double retrievalRunnerUpScore = 0.0;
    int retrievalTileCount = 0;
    double retrievalMilliseconds = 0.0;
};

// The cold-start ranking's own numbers as one diagnostic suffix, leading with its own space. It
// lives beside the result it describes so whoever reads the log can find what produced it.
inline std::string MapViewportRetrievalFields(const MapViewportLocalizationResult& result) {
    return " retrievalRanked=" + std::to_string(result.retrievalRankedSceneCount) +
        " retrievalScenes=" + std::to_string(result.retrievalSceneCount) +
        " retrievalScene=" + std::to_string(result.retrievalSceneId) +
        " retrievalTop=" + std::to_string(result.retrievalTopScore) +
        " retrievalRunnerUp=" + std::to_string(result.retrievalRunnerUpScore) +
        " retrievalTiles=" + std::to_string(result.retrievalTileCount) +
        " retrievalMs=" + std::to_string(result.retrievalMilliseconds);
}

// Dedicated worker for the full-screen map.  It intentionally does not share
// GlobalVisualLocalizer's worker: a slow full-map viewport search must never
// delay minimap recovery or the main capture loop.
class MapViewportLocalizer {
public:
    static bool Initialize(std::shared_ptr<const RuntimeFeatureResources> resources, std::string& error);
    static void Shutdown();
    static void CancelPending();
    static bool IsReady();
    static bool Submit(MapViewportLocalizationRequest request);
    static bool TryTakeLatestResult(MapViewportLocalizationResult& result);
    static const char* ScopeName(MapViewportSearchScope scope);
};
