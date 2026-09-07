#pragma once

#include "../../Feature/Match/FeatureMatch.h"
#include "../../Feature/RuntimeFeatureRepository.h"
#include "../CoordinateStruct.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

enum class VisualLocalizationQuality {
    Rejected,
    Marginal,
    Strong
};

struct VisualMapHint {
    int sceneId = 0;
    Coordinate mapCoordinate;
};

struct VisualLocalizationCandidate {
    int sceneId = 0;
    Coordinate mapCenter;
    double rotationDegrees = 0.0;
    double scale = 0.0;
    int mutualMatchCount = 0;
    int inlierCount = 0;
    double inlierRatio = 0.0;
    double medianReprojectionError = 0.0;
    int coveredQuadrants = 0;
    bool affineEstimated = false;
    bool scaleWithinExpectedRange = false;
    double retrievalScore = 0.0;
    // Only populated by the fixed-scale translation-vote fallback. Higher is
    // better and represents the median descriptor agreement of its support.
    double translationDescriptorScore = 0.0;
    bool ocrHintMatched = false;
    VisualLocalizationQuality quality = VisualLocalizationQuality::Rejected;
};

struct VisualLocalizationRequest {
    std::uint64_t sessionId = 0;
    std::uint64_t uiGeneration = 0;
    std::uint64_t frameId = 0;
    // A recovery can submit a hint-scoped retry while an older global request
    // is still running.  App accepts only the active request id.
    std::uint64_t requestId = 0;
    std::uint64_t hintVersion = 0;
    cv::Mat normalizedMinimap;
    ImageFeatureData minimapFeatures;
    std::vector<VisualMapHint> ocrHints;
    bool requireOcrHint = false;
};

struct VisualLocalizationResult {
    std::uint64_t sessionId = 0;
    std::uint64_t uiGeneration = 0;
    std::uint64_t frameId = 0;
    std::uint64_t requestId = 0;
    std::uint64_t hintVersion = 0;
    VisualLocalizationQuality quality = VisualLocalizationQuality::Rejected;
    bool ambiguous = false;
    bool searchIncomplete = false;
    double recoveryAngle = 0.0;
    double attemptedRecoveryAngle = 0.0;
    // Recovery visits later ranked regions on subsequent frames. Counts refer
    // to the unrotated query, even when an optional rotation is interrupted.
    std::size_t coarseSearchOffset = 0;
    std::size_t totalCoarseCandidates = 0;
    std::size_t verifiedCoarseCandidates = 0;
    std::size_t nextCoarseSearchOffset = 0;
    bool usedCandidateHint = false;
    std::vector<VisualLocalizationCandidate> candidates;
    std::size_t coarseCandidateCount = 0;
    int bestMutualMatchCount = 0;
    int bestInlierCount = 0;
    double bestObservedScale = 0.0;
    bool bestAffineEstimated = false;
    bool bestScaleWithinExpectedRange = false;
    double bestRetrievalScore = 0.0;
    double coarseMilliseconds = 0.0;
    double verificationMilliseconds = 0.0;
    double totalMilliseconds = 0.0;
};

struct MinimapFeatureDiagnostics {
    int rawKeypointCount = 0;
    int retainedKeypointCount = 0;
    int dynamicMaskPercent = 0;
    bool temporalMaskApplied = false;
    cv::Mat featureMask;
};

class GlobalVisualLocalizer {
public:
    static bool Initialize(std::shared_ptr<const RuntimeFeatureResources> resources, std::string& error);
    static void Shutdown();
    static bool IsReady();

    // trustedReference is the normalized crop from the most recent confirmed
    // player position. When it can be registered reliably, dynamic HUD and
    // transparent-background pixels are removed from the feature mask.
    static bool PrepareMinimap(const cv::Mat& minimap, cv::Mat& normalized,
        ImageFeatureData& features, const cv::Mat* trustedReference = nullptr,
        MinimapFeatureDiagnostics* diagnostics = nullptr, double surfThreshold = 60.0);

    static bool Submit(VisualLocalizationRequest request);
    static bool TryTakeLatestResult(VisualLocalizationResult& result);
    static void CancelPending();

    static VisualLocalizationResult LocateForDiagnostics(
        std::shared_ptr<const RuntimeFeatureResources> resources,
        const VisualLocalizationRequest& request);

    // Freeze a scale calibrated by an independently verified acquisition for
    // the whole local-tracking interval; do not update it from each local fit.
    static bool TrackLocal(const cv::Mat& normalizedMinimap,
        const ImageFeatureData& minimapFeatures, int sceneId,
        const Coordinate& previousMapCenter,
        VisualLocalizationCandidate& result,
        double fixedTerrainScale = 194.0 / 184.0);

    // Revalidates a recently known area after returning from a full-screen UI.
    // Unlike TrackLocal this may inspect neighbouring map tiles, but it never
    // becomes a position until the caller validates the current minimap frame.
    static bool TrackNearby(const cv::Mat& normalizedMinimap,
        const ImageFeatureData& minimapFeatures, int sceneId,
        const Coordinate& previousMapCenter, double searchRadius,
        VisualLocalizationCandidate& result);

    static std::string_view QualityName(VisualLocalizationQuality quality);
};
