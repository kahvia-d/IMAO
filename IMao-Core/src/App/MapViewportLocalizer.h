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
    //
    // These describe ONE ranking, never a mixture. When a plan answers they are that plan's own
    // ranking; only when nothing answers are they the most decisive of the four, which is then the
    // whole of what there is to report. The distinction matters because a ranking is per zoom - the
    // same crop puts a different scene first at a different scale - so a field assembled from two
    // rankings answers no question at all, and `retrievalScene` disagreeing with `scene` reads as a
    // ranking error whether or not one occurred.
    int retrievalRankedSceneCount = 0;
    int retrievalSceneCount = 0;
    int retrievalSceneId = 0;
    // The zoom the reported ranking was taken at. The ranking is only interpretable with it: a crop
    // scaled away from the zoom the index was built at ranks the largest scene first regardless of
    // where the player is, which is a property of the scale and not of the frame.
    double retrievalFactor = 1.0;
    // The runner-up's identity, not just its score. Whether the confusion is a neighbouring region
    // or an unrelated one is what says whether the lead threshold should be loosened - and the
    // first field session had three searches miss it by less than 0.06.
    int retrievalRunnerUpSceneId = 0;
    // The winner's lead, top score over the runner-up's - NOT the winner's own score. It is what
    // orders the four rankings and what says how decisively this one separated its winner.
    double retrievalTopScore = 0.0;
    double retrievalRunnerUpScore = 0.0;
    int retrievalTileCount = 0;
    double retrievalMilliseconds = 0.0;
    // Which plan answered, and how many ran; -1 means none accepted. Plans 0 and up are the ranked
    // plans, one per zoom, most decisive zoom first; after them come the wider rung and then the
    // windowed sweep. Without this the difference between "the ranking worked" and "the ranking
    // missed and something below rescued it" has to be inferred from how many matchers were built,
    // which is a guess dressed as a measurement.
    int plansRun = 0;
    int acceptedPlan = -1;
    // How far plan 0's own candidates sat from the position this search accepted: the nearest and
    // the farthest, in map units, measured to the tiles' rectangles. Only an accepted search can
    // measure it, and it is what separates "the candidates never reached the answer" from "they
    // reached it and the match failed anyway".
    double candidateNearestM = 0.0;
    double candidateReachM = 0.0;
};

// The cold-start ranking's own numbers as one diagnostic suffix, leading with its own space. It
// lives beside the result it describes so whoever reads the log can find what produced it.
inline std::string MapViewportRetrievalFields(const MapViewportLocalizationResult& result) {
    return " retrievalRanked=" + std::to_string(result.retrievalRankedSceneCount) +
        " retrievalScenes=" + std::to_string(result.retrievalSceneCount) +
        " retrievalScene=" + std::to_string(result.retrievalSceneId) +
        " retrievalFactor=" + std::to_string(result.retrievalFactor) +
        " retrievalRunnerUpScene=" + std::to_string(result.retrievalRunnerUpSceneId) +
        " retrievalTop=" + std::to_string(result.retrievalTopScore) +
        " retrievalRunnerUp=" + std::to_string(result.retrievalRunnerUpScore) +
        " retrievalTiles=" + std::to_string(result.retrievalTileCount) +
        " retrievalMs=" + std::to_string(result.retrievalMilliseconds) +
        " plansRun=" + std::to_string(result.plansRun) +
        " acceptedPlan=" + std::to_string(result.acceptedPlan) +
        " candidateNearestM=" + std::to_string(result.candidateNearestM) +
        " candidateReachM=" + std::to_string(result.candidateReachM);
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
