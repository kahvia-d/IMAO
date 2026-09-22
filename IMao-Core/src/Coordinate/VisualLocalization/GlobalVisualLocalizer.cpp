#include <iostream>
#include "GlobalVisualLocalizer.h"

#include "../../Diagnostics/Diagnostics.h"
#include "../../Runtime/ThreadPriority.h"
#include "RecoveryPolicy.h"
#include "CoarseSearchCoverage.h"
#include "MinimapTrackingGeometry.h"
#include "MinimapTerrainEvidence.h"
#include "../../Feature/Match/UniqueMapFeatures.h"
#include "../../Feature/Match/FeatureRowCache.h"
#include "../../Feature/Match/ExactDescriptorMatcher.h"

#include <opencv2/calib3d.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/flann.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/xfeatures2d.hpp>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <span>
#include <thread>
#include <tuple>
#include <unordered_map>

namespace {
constexpr std::size_t kMaximumCoarseCandidates = 12;
constexpr double kExpectedScale = 194.0 / 184.0;
constexpr double kMaximumScaleDeviation = 0.15;
constexpr double kDuplicateCenterDistance = 24.0;
constexpr double kOcrHintDistance = 160.0;
constexpr double kOcrHintSearchRadius = 384.0;
// These values must stay aligned with KuroMapFeatureBuilder::VerifyReference.
// The public-map descriptor pack is built from an 80 px terrain ring with
// SURF's 60 threshold.  A looser runtime detector produced a large number of
// HUD/anti-aliasing descriptors that were absent from the pack, so retrieval
// could miss an otherwise exact map tile before geometric verification began.
constexpr int kMinimapTerrainRadius = 80;
constexpr int kPlayerMarkerMaskRadius = 42;
constexpr double kMinimumTemporalRegistrationResponse = 0.12;
constexpr double kMaximumTemporalRegistrationShift = 16.0;
constexpr int kDynamicDescriptorPadding = 11;
constexpr double kMinimapSurfThreshold = 60.0;
constexpr float kDescriptorRatioThreshold = 0.72f;
constexpr float kDescriptorDistanceThreshold = 0.60f;
// The public Kuro tiles and the in-game minimap share a known north-up scale.
// A global affine fit is unnecessarily fragile when only a few terrain details
// survive the minimap mask, so retain the stricter descriptor gate used by the
// field verifier and vote directly for the map position implied by each match.
constexpr float kTranslationVoteRatioThreshold = 0.65f;
constexpr float kTranslationVoteDistanceThreshold = 0.50f;
constexpr double kTranslationVoteRadius = 8.0;
constexpr int kMinimumTranslationVotes = 3;

cv::Mat ToGray(const cv::Mat& image) {
    cv::Mat gray;
    if (image.channels() == 1) gray = image;
    else if (image.channels() == 4) cv::cvtColor(image, gray, cv::COLOR_BGRA2GRAY);
    else cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    return gray;
}

bool ExtractPreparedMinimapFeatures(const cv::Mat& normalized, ImageFeatureData& features,
    const cv::Mat* trustedReference, MinimapFeatureDiagnostics* diagnostics,
    double surfThreshold = kMinimapSurfThreshold) {
    if (normalized.empty()) return false;
    if (diagnostics != nullptr) *diagnostics = {};
    const cv::Mat gray = ToGray(normalized);
    cv::Mat baseMask(gray.size(), CV_8UC1, cv::Scalar(0));
    const cv::Point center(gray.cols / 2, gray.rows / 2);
    cv::circle(baseMask, center, kMinimapTerrainRadius, cv::Scalar(255), cv::FILLED);
    // The player-arrow core and the start of the direction cone are dynamic.
    // The remaining cone/background pixels are removed below when a recent
    // trusted frame can be registered to this crop.
    cv::circle(baseMask, center, kPlayerMarkerMaskRadius, cv::Scalar(0), cv::FILLED);
    auto surf = cv::xfeatures2d::SURF::create(surfThreshold, 8, 4, true, true);
    std::vector<cv::KeyPoint> rawKeypoints;
    // Counting keypoints is intentionally cheaper than producing a second
    // descriptor set. The latter is needed only for the rare fallback where
    // the temporal mask rejected every usable feature.
    surf->detect(gray, rawKeypoints, baseMask);
    if (rawKeypoints.size() < 24 && normalized.channels() >= 3) {
        baseMask = MinimapTerrainEvidence::TerrainMask(normalized);
        surf->detect(gray, rawKeypoints, baseMask);
        if (diagnostics != nullptr) diagnostics->adaptivePlayerMaskApplied = true;
    }
    if (diagnostics != nullptr) diagnostics->rawKeypointCount = static_cast<int>(rawKeypoints.size());

    cv::Mat mask = baseMask.clone();
    if (trustedReference != nullptr && !trustedReference->empty() &&
        trustedReference->size() == normalized.size()) {
        const cv::Mat referenceGray = ToGray(*trustedReference);
        cv::Mat referenceFloat;
        cv::Mat currentFloat;
        referenceGray.convertTo(referenceFloat, CV_32F);
        gray.convertTo(currentFloat, CV_32F);
        // Restrict phase correlation to the same static terrain annulus used
        // for feature detection. A reliable small translation preserves map
        // terrain while exposing the rotating cone and transparent backdrop.
        referenceFloat.setTo(0.0f, baseMask == 0);
        currentFloat.setTo(0.0f, baseMask == 0);
        cv::Mat window;
        cv::createHanningWindow(window, gray.size(), CV_32F);
        double response = 0.0;
        const cv::Point2d shift = cv::phaseCorrelate(referenceFloat, currentFloat, window, &response);
        if (response >= kMinimumTemporalRegistrationResponse &&
            cv::norm(shift) <= kMaximumTemporalRegistrationShift) {
            cv::Mat alignedReference;
            const cv::Mat transform = (cv::Mat_<double>(2, 3) << 1.0, 0.0, shift.x, 0.0, 1.0, shift.y);
            cv::warpAffine(referenceGray, alignedReference, transform, gray.size(), cv::INTER_LINEAR,
                cv::BORDER_REPLICATE);
            cv::Mat difference;
            cv::absdiff(gray, alignedReference, difference);
            cv::Scalar mean;
            cv::Scalar deviation;
            cv::meanStdDev(difference, mean, deviation, baseMask);
            const double threshold = std::clamp(mean[0] + deviation[0] * 1.25, 20.0, 80.0);
            cv::Mat dynamicMask;
            cv::threshold(difference, dynamicMask, threshold, 255, cv::THRESH_BINARY);
            cv::bitwise_and(dynamicMask, baseMask, dynamicMask);
            const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE,
                cv::Size(kDynamicDescriptorPadding, kDynamicDescriptorPadding));
            cv::dilate(dynamicMask, dynamicMask, kernel);
            cv::bitwise_and(dynamicMask, baseMask, dynamicMask);
            const int terrainPixels = cv::countNonZero(baseMask);
            const int dynamicPixels = cv::countNonZero(dynamicMask);
            if (diagnostics != nullptr) {
                diagnostics->temporalMaskApplied = true;
                diagnostics->dynamicMaskPercent = terrainPixels > 0
                    ? static_cast<int>(std::lround(dynamicPixels * 100.0 / terrainPixels)) : 0;
            }
            mask.setTo(0, dynamicMask);
        }
    }

    std::vector<cv::KeyPoint> retainedKeypoints;
    cv::Mat retainedDescriptors;
    surf->detectAndCompute(gray, mask, retainedKeypoints, retainedDescriptors);
    // Moving terrain must not leave a nonempty but unusably sparse request.
    // Retain the fixed player mask; geometry still validates every position.
    if (ShouldRetryWithoutTemporalMask(rawKeypoints.size(), retainedKeypoints.size())) {
        mask = baseMask;
        retainedKeypoints = rawKeypoints;
        surf->compute(gray, retainedKeypoints, retainedDescriptors);
        features = ImageFeatureData(retainedKeypoints, retainedDescriptors);
        if (diagnostics != nullptr) {
            diagnostics->temporalMaskApplied = false;
            diagnostics->dynamicMaskPercent = 0;
        }
    }
    else {
        features = ImageFeatureData(retainedKeypoints, retainedDescriptors);
    }
    if (diagnostics != nullptr) {
        diagnostics->retainedKeypointCount = static_cast<int>(retainedKeypoints.size());
        diagnostics->featureMask = mask;
    }
    return !features.imgDescriptors.empty();
}

double ElapsedMilliseconds(const std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

double Distance(const Coordinate& left, const Coordinate& right) {
    return std::hypot(left.x - right.x, left.y - right.y);
}

bool TileMatchesHint(const MapVisualTile& tile, int resultSceneId,
    const std::vector<VisualMapHint>& hints) {
    for (const auto& hint : hints) {
        if (hint.sceneId != resultSceneId) continue;
        const double nearestX = std::clamp(hint.mapCoordinate.x,
            static_cast<double>(tile.minX), static_cast<double>(tile.maxX));
        const double nearestY = std::clamp(hint.mapCoordinate.y,
            static_cast<double>(tile.minY), static_cast<double>(tile.maxY));
        if (std::hypot(hint.mapCoordinate.x - nearestX,
            hint.mapCoordinate.y - nearestY) <= kOcrHintSearchRadius) return true;
    }
    return false;
}

struct TranslationVote {
    cv::Point2d mapCenter;
    cv::Point2f minimapPoint;
    float descriptorDistance = 0.0f;
};

double Median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2;
    return values.size() % 2 == 0
        ? (values[middle - 1] + values[middle]) / 2.0
        : values[middle];
}

VisualLocalizationCandidate BuildTranslationVoteCandidate(const std::vector<TranslationVote>& votes,
    const cv::Size minimapSize, int sceneId, double retrievalScore, int mutualMatchCount,
    double translationScale = 0.0) {
    if (translationScale <= 0.0) translationScale = Scene::MinimapScale(sceneId);
    VisualLocalizationCandidate candidate;
    candidate.sceneId = sceneId;
    candidate.retrievalScore = retrievalScore;
    candidate.mutualMatchCount = mutualMatchCount;
    if (votes.size() < static_cast<std::size_t>(kMinimumTranslationVotes)) return candidate;

    std::vector<std::size_t> bestSupport;
    double bestDistanceSum = std::numeric_limits<double>::infinity();
    for (std::size_t anchor = 0; anchor < votes.size(); ++anchor) {
        std::vector<std::size_t> support;
        double distanceSum = 0.0;
        for (std::size_t index = 0; index < votes.size(); ++index) {
            const double distance = std::hypot(votes[index].mapCenter.x - votes[anchor].mapCenter.x,
                votes[index].mapCenter.y - votes[anchor].mapCenter.y);
            if (distance > kTranslationVoteRadius) continue;
            support.push_back(index);
            distanceSum += distance;
        }
        if (support.size() > bestSupport.size() ||
            (support.size() == bestSupport.size() && distanceSum < bestDistanceSum)) {
            bestSupport = std::move(support);
            bestDistanceSum = distanceSum;
        }
    }
    if (bestSupport.size() < static_cast<std::size_t>(kMinimumTranslationVotes)) return candidate;

    std::vector<double> centerXs;
    std::vector<double> centerYs;
    centerXs.reserve(bestSupport.size());
    centerYs.reserve(bestSupport.size());
    for (const auto index : bestSupport) {
        centerXs.push_back(votes[index].mapCenter.x);
        centerYs.push_back(votes[index].mapCenter.y);
    }
    candidate.mapCenter = { Median(std::move(centerXs)), Median(std::move(centerYs)) };

    std::vector<double> residuals;
    std::vector<double> descriptorDistances;
    std::array<bool, 4> quadrants{};
    const cv::Point2d minimapCenter(minimapSize.width / 2.0, minimapSize.height / 2.0);
    for (const auto index : bestSupport) {
        const auto& vote = votes[index];
        const double residual = std::hypot(vote.mapCenter.x - candidate.mapCenter.x,
            vote.mapCenter.y - candidate.mapCenter.y);
        if (residual > kTranslationVoteRadius) continue;
        residuals.push_back(residual);
        descriptorDistances.push_back(vote.descriptorDistance);
        const int quadrant = (vote.minimapPoint.x >= minimapCenter.x ? 1 : 0) +
            (vote.minimapPoint.y >= minimapCenter.y ? 2 : 0);
        quadrants[quadrant] = true;
    }
    if (residuals.size() < static_cast<std::size_t>(kMinimumTranslationVotes)) return VisualLocalizationCandidate{};

    candidate.sceneId = sceneId;
    candidate.retrievalScore = retrievalScore;
    candidate.mutualMatchCount = mutualMatchCount;
    candidate.inlierCount = static_cast<int>(residuals.size());
    candidate.inlierRatio = static_cast<double>(candidate.inlierCount) / static_cast<double>(votes.size());
    candidate.medianReprojectionError = Median(std::move(residuals));
    candidate.translationDescriptorScore = 1.0 / (1.0 + Median(std::move(descriptorDistances)));
    candidate.coveredQuadrants = static_cast<int>(std::count(quadrants.begin(), quadrants.end(), true));
    candidate.scale = translationScale;
    candidate.scaleWithinExpectedRange = true;
    // This is intentionally a fixed-scale translation estimate, not a failed
    // affine estimate. App requires an independent next frame before using it.
    candidate.affineEstimated = false;
    candidate.quality = VisualLocalizationQuality::Marginal;
    return candidate;
}

std::vector<VisualLocalizationCandidate> BuildTranslationVoteCandidates(std::vector<TranslationVote> votes,
    const cv::Size minimapSize, int sceneId, double retrievalScore, int mutualMatchCount) {
    std::vector<VisualLocalizationCandidate> candidates;
    candidates.reserve(kMaximumCoarseCandidates);
    while (votes.size() >= static_cast<std::size_t>(kMinimumTranslationVotes) &&
        candidates.size() < kMaximumCoarseCandidates) {
        auto candidate = BuildTranslationVoteCandidate(votes, minimapSize, sceneId,
            retrievalScore, mutualMatchCount);
        if (candidate.inlierCount < kMinimumTranslationVotes) break;
        candidates.push_back(candidate);
        votes.erase(std::remove_if(votes.begin(), votes.end(), [&](const auto& vote) {
            return std::hypot(vote.mapCenter.x - candidate.mapCenter.x,
                vote.mapCenter.y - candidate.mapCenter.y) <= kDuplicateCenterDistance;
        }), votes.end());
    }
    return candidates;
}

int QualityRank(VisualLocalizationQuality quality) {
    return static_cast<int>(quality);
}

bool BetterCandidate(const VisualLocalizationCandidate& left, const VisualLocalizationCandidate& right) {
    if (QualityRank(left.quality) != QualityRank(right.quality)) {
        return QualityRank(left.quality) > QualityRank(right.quality);
    }
    if (left.ocrHintMatched != right.ocrHintMatched) return left.ocrHintMatched;
    if (left.affineEstimated != right.affineEstimated) return left.affineEstimated;
    // A fixed-scale translation vote has no fitted degrees of freedom. Its
    // residual is therefore the decisive evidence; descriptor score and vote
    // count mainly reflect how many unrelated map features were considered.
    if (!left.affineEstimated && !right.affineEstimated) {
        if (left.medianReprojectionError != right.medianReprojectionError) {
            return left.medianReprojectionError < right.medianReprojectionError;
        }
        if (left.translationDescriptorScore != right.translationDescriptorScore) {
            return left.translationDescriptorScore > right.translationDescriptorScore;
        }
    }
    if (left.inlierCount != right.inlierCount) return left.inlierCount > right.inlierCount;
    if (left.inlierRatio != right.inlierRatio) return left.inlierRatio > right.inlierRatio;
    if (left.medianReprojectionError != right.medianReprojectionError) {
        return left.medianReprojectionError < right.medianReprojectionError;
    }
    if (left.retrievalScore != right.retrievalScore) return left.retrievalScore > right.retrievalScore;
    return std::tie(left.sceneId, left.mapCenter.y, left.mapCenter.x) <
        std::tie(right.sceneId, right.mapCenter.y, right.mapCenter.x);
}

class VisualLocalizationEngine {
    struct SearchProgress {
        std::size_t offset = 0, total = 0, verified = 0, next = 0;
        std::vector<uint32_t> rankedRegions;
    };
    struct MatchedRows {
        std::shared_ptr<const ImageFeatureData> subset;
        std::vector<std::vector<cv::DMatch>> forward, reverse;
    };
public:
    explicit VisualLocalizationEngine(std::shared_ptr<const RuntimeFeatureResources> resources)
        : resources_(std::move(resources)), rowCache_(resources_->map), vocabularyIndex_(
            resources_->visualIndex.vocabulary, cv::flann::KDTreeIndexParams(4)) {
    }

    VisualLocalizationResult Locate(const VisualLocalizationRequest& request, double recoveryAngle = 0.0,
        const std::function<bool()>& interrupted = {}, CoarseSearchCoverage* coverage = nullptr) const {
        const auto start = std::chrono::steady_clock::now();
        VisualLocalizationResult result;
        result.sessionId = request.sessionId; result.uiGeneration = request.uiGeneration;
        result.frameId = request.frameId; result.requestId = request.requestId; result.hintVersion = request.hintVersion;
        SearchProgress progress;
        try {
            // Every frame first gets a chance in its real orientation. Expensive
            // rotation recovery advances across frames instead of monopolizing a worker.
            // Later pages must not repeat complete optional-pack scans. They
            // have the same budget as page zero and include the base map too.
            result = LocateOnce(request, interrupted,
                request.requireOcrHint || recoveryAngle == 0.0,
                request.requireOcrHint ? nullptr : coverage, &progress);
            const bool supported = std::any_of(result.candidates.begin(), result.candidates.end(), [](const auto& value) {
                return value.quality != VisualLocalizationQuality::Rejected &&
                    HasReacquisitionSupport(value.affineEstimated, value.inlierCount, value.inlierRatio, value.coveredQuadrants);
            });
            if (!supported && recoveryAngle != 0.0 && !request.requireOcrHint && !request.normalizedMinimap.empty()) {
                if (interrupted && interrupted()) throw SearchInterrupted{};
                VisualLocalizationRequest rotated = request;
                const cv::Point2f center(request.normalizedMinimap.cols / 2.0f, request.normalizedMinimap.rows / 2.0f);
                cv::warpAffine(request.normalizedMinimap, rotated.normalizedMinimap,
                    cv::getRotationMatrix2D(center, recoveryAngle, 1.0), request.normalizedMinimap.size(),
                    cv::INTER_LINEAR, cv::BORDER_REFLECT_101);
                if (ExtractPreparedMinimapFeatures(rotated.normalizedMinimap, rotated.minimapFeatures, nullptr, nullptr)) {
                    auto recovered = LocateOnce(rotated, interrupted, false);
                    recovered.recoveryAngle = recoveryAngle;
                    // Rotation recovery still requires strong geometry; translation votes cannot publish.
                    if (recovered.quality == VisualLocalizationQuality::Strong) {
                        for (auto& candidate : recovered.candidates) {
                            candidate.rotationDegrees -= recoveryAngle;
                            while (candidate.rotationDegrees > 180.0) candidate.rotationDegrees -= 360.0;
                            while (candidate.rotationDegrees <= -180.0) candidate.rotationDegrees += 360.0;
                        }
                        result = std::move(recovered);
                    }
                }
            }
            if (coverage) coverage->CompletePage();
        }
        catch (const SearchInterrupted&) {
            // An interrupted page cannot publish its candidates. Keep their
            // regions eligible so a later frame can validate them again.
            if (coverage) { coverage->InterruptPage(); progress.next = coverage->NextRank(progress.rankedRegions); }
            result.quality = VisualLocalizationQuality::Rejected;
            result.candidates.clear();
            result.searchIncomplete = true;
        }
        result.totalMilliseconds = ElapsedMilliseconds(start);
        result.attemptedRecoveryAngle = recoveryAngle;
        result.coarseSearchOffset = progress.offset;
        result.totalCoarseCandidates = progress.total;
        result.verifiedCoarseCandidates = progress.verified;
        result.nextCoarseSearchOffset = progress.next;
        return result;
    }

    VisualLocalizationResult LocateOnce(const VisualLocalizationRequest& request,
        const std::function<bool()>& interrupted, bool fullShardRecovery,
        CoarseSearchCoverage* coverage = nullptr, SearchProgress* progress = nullptr) const {
        VisualLocalizationResult result;
        result.sessionId = request.sessionId;
        result.uiGeneration = request.uiGeneration;
        result.frameId = request.frameId;
        result.requestId = request.requestId;
        result.hintVersion = request.hintVersion;
        const auto totalStart = std::chrono::steady_clock::now();
        if (request.minimapFeatures.imgDescriptors.empty() ||
            request.minimapFeatures.imgDescriptors.type() != CV_32FC1 ||
            request.minimapFeatures.imgDescriptors.cols != static_cast<int>(MapVisualIndex::DescriptorColumns)) {
            result.totalMilliseconds = ElapsedMilliseconds(totalStart);
            return result;
        }

        const auto coarseStart = std::chrono::steady_clock::now();
        const auto rankedCandidates = RetrieveTiles(request.minimapFeatures.imgDescriptors,
            request.requireOcrHint ? &request.ocrHints : nullptr);
        std::vector<uint32_t> rankedRegions;
        rankedRegions.reserve(rankedCandidates.size());
        for (const auto& value : rankedCandidates) rankedRegions.push_back(value.first);
        std::vector<std::pair<uint32_t, double>> coarseCandidates;
        if (coverage) {
            const auto page = coverage->BeginPage(resources_->visualIndex.tiles.size(), rankedRegions, kMaximumCoarseCandidates);
            fullShardRecovery = page.startsNewCycle;
            for (const auto rank : page.ranks) coarseCandidates.push_back(rankedCandidates[rank]);
            if (progress && !page.ranks.empty()) progress->offset = page.ranks.front();
        }
        else {
            const auto count = std::min(kMaximumCoarseCandidates, rankedCandidates.size());
            coarseCandidates.assign(rankedCandidates.begin(), rankedCandidates.begin() + count);
        }
        if (progress) {
            progress->total = rankedCandidates.size(); progress->next = progress->offset;
            progress->rankedRegions = rankedRegions;
        }
        result.coarseCandidateCount = coarseCandidates.size();
        if (!coarseCandidates.empty()) result.bestRetrievalScore = coarseCandidates.front().second;
        result.coarseMilliseconds = ElapsedMilliseconds(coarseStart);

        const auto verificationStart = std::chrono::steady_clock::now();
        std::vector<VisualLocalizationCandidate> candidates;
        candidates.reserve(coarseCandidates.size());
        const auto recordVerification = [&](const VisualLocalizationCandidate& candidate) {
            if (candidate.mutualMatchCount < result.bestMutualMatchCount ||
                (candidate.mutualMatchCount == result.bestMutualMatchCount &&
                    candidate.inlierCount < result.bestInlierCount)) return;
            result.bestMutualMatchCount = candidate.mutualMatchCount;
            result.bestInlierCount = candidate.inlierCount;
            result.bestObservedScale = candidate.scale;
            result.bestAffineEstimated = candidate.affineEstimated;
            result.bestScaleWithinExpectedRange = candidate.scaleWithinExpectedRange;
        };
        for (const auto& [tileIndex, score] : coarseCandidates) {
            if (interrupted && interrupted()) throw SearchInterrupted{};
            const auto& tile = resources_->visualIndex.tiles[tileIndex];
            std::vector<std::uint32_t> rows;
            const int resultSceneId = tileIndex < resources_->baseVisualTileCount ? 1 : tile.sceneId;
            for (std::size_t compatibleIndex = 0; compatibleIndex < resources_->visualIndex.tiles.size(); ++compatibleIndex) {
                const auto& compatible = resources_->visualIndex.tiles[compatibleIndex];
                const int compatibleSceneId = compatibleIndex < resources_->baseVisualTileCount ? 1 : compatible.sceneId;
                if (compatibleSceneId != resultSceneId || compatible.gridX != tile.gridX ||
                    compatible.gridY != tile.gridY) continue;
                rows.insert(rows.end(),
                    resources_->visualIndex.featureRows.begin() + compatible.featureRowOffset,
                    resources_->visualIndex.featureRows.begin() + compatible.featureRowOffset +
                        compatible.featureRowCount);
            }
            std::sort(rows.begin(), rows.end());
            rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
            // Unattributed legacy atlas rows retain the historical World fallback.
            // Its tiles were historically partitioned by nearest scene origin to
            // improve retrieval, which puts Black Shores near Tethys internally.
            // Normalize this legacy identity for grouping and matching too;
            // Hash-bound replacement metadata retires identified non-World duplicates.
            if (request.requireOcrHint && !TileMatchesHint(tile, resultSceneId, request.ocrHints)) {
                continue;
            }
            VisualLocalizationCandidate candidate = VerifyRows(rows, request.minimapFeatures,
                request.normalizedMinimap.size(), resultSceneId, score, request.ocrHints, interrupted);
            if (coverage) coverage->RecordVerified(tileIndex,
                candidate.quality != VisualLocalizationQuality::Rejected &&
                HasReacquisitionSupport(candidate.affineEstimated, candidate.inlierCount,
                    candidate.inlierRatio, candidate.coveredQuadrants));
            if (progress) {
                ++progress->verified;
                if (coverage) progress->next = coverage->NextRank(rankedRegions);
            }
            recordVerification(candidate);
            const bool fixedScaleTranslation = candidate.quality == VisualLocalizationQuality::Marginal &&
                !candidate.affineEstimated && candidate.inlierCount >= kMinimumTranslationVotes &&
                candidate.coveredQuadrants >= 2;
            // A candidate without an affine fit is a fixed-scale translation
            // vote.  Do not let its raw inlier count bypass the independent
            // terrain-support requirement: four (or more) descriptors from
            // one translucent HUD corner can otherwise survive here and be
            // confirmed by the next identical frame as a false location.
            const bool geometricCandidate = candidate.affineEstimated && candidate.inlierCount >= 4;
            if (geometricCandidate || fixedScaleTranslation) candidates.push_back(candidate);
        }

        // Public map additions can contain visual details that were absent from
        // the historical base map used to train the retrieval vocabulary.  In
        // that case the coarse search may not rank the correct scene tile high
        // enough to reach verification at all.  When it found no geometric
        // candidate, make one bounded exact pass over optional scene features.
        // This remains image-only: it does not use the game's coordinate text.
        const bool hasSupportedGeometricCandidate = std::any_of(candidates.begin(), candidates.end(),
            [](const auto& candidate) {
                return candidate.quality != VisualLocalizationQuality::Rejected &&
                    HasReacquisitionSupport(candidate.affineEstimated, candidate.inlierCount,
                        candidate.inlierRatio, candidate.coveredQuadrants);
            });
        // A two-quadrant affine candidate can already be revalidated nearby.
        // Scanning every full shard again cannot supply an independent frame;
        // it only delays publication and repeats the same observations.
        if (fullShardRecovery && !hasSupportedGeometricCandidate && !resources_->kuroVisualShards.empty()) {
            for (const auto& shard : resources_->kuroVisualShards) {
                if (interrupted && interrupted()) throw SearchInterrupted{};
                if (!Scene::IsRuntimeApproved(shard.sceneId) || shard.tileCount == 0 ||
                    shard.firstTile > resources_->visualIndex.tiles.size() ||
                    shard.tileCount > resources_->visualIndex.tiles.size() - shard.firstTile) continue;
                std::vector<std::uint32_t> sceneRows;
                for (std::uint32_t tileIndex = shard.firstTile;
                    tileIndex < shard.firstTile + shard.tileCount; ++tileIndex) {
                    const auto& tile = resources_->visualIndex.tiles[tileIndex];
                    if (request.requireOcrHint && !TileMatchesHint(tile, shard.sceneId, request.ocrHints)) {
                        continue;
                    }
                    sceneRows.insert(sceneRows.end(),
                        resources_->visualIndex.featureRows.begin() + tile.featureRowOffset,
                        resources_->visualIndex.featureRows.begin() + tile.featureRowOffset +
                            tile.featureRowCount);
                }
                std::sort(sceneRows.begin(), sceneRows.end());
                sceneRows.erase(std::unique(sceneRows.begin(), sceneRows.end()), sceneRows.end());
                if (sceneRows.size() < 4) continue;
                const auto sharedMatches = MatchRows(sceneRows, request.minimapFeatures, interrupted);
                VisualLocalizationCandidate candidate = VerifyRows(sceneRows, request.minimapFeatures,
                    request.normalizedMinimap.size(), shard.sceneId, 0.0, request.ocrHints, interrupted, &sharedMatches);
                recordVerification(candidate);
                // VerifyRows already fitted these observations. A Strong fit
                // outranks every translation-only vote, so do not repeat the
                // full descriptor search merely to generate weaker copies.
                // Other shards still undergo geometry checks for ambiguity.
                const auto translations = candidate.quality == VisualLocalizationQuality::Strong
                    ? std::vector<VisualLocalizationCandidate>{}
                    : VerifyTranslationRows(sceneRows, request.minimapFeatures,
                        request.normalizedMinimap.size(), shard.sceneId, 0.0, interrupted, &sharedMatches);
                for (auto translation : translations) {
                    recordVerification(translation);
                    // Three descriptors from one corner of a translucent HUD
                    // can repeatedly vote for the same unrelated map point.
                    // A translation-only candidate has no fitted geometry to
                    // reject that coincidence, so require independent terrain
                    // support on both sides of the minimap before it can enter
                    // the publication/nearby-validation path.
                    if (translation.coveredQuadrants >= 2) candidates.push_back(std::move(translation));
                }
                // A newly added public-map area can have fewer retained SURF
                // features than the legacy map.  Four mutually consistent
                // matches are useful only in this bounded fallback, and only
                // when their geometry is exceptionally tight.  Mark it as
                // marginal so callers still require a second frame before
                // treating it as a fully confirmed position.
                const bool lowDensityButStable = candidate.inlierCount >= 4 &&
                    candidate.inlierRatio >= 0.25 && candidate.coveredQuadrants >= 2 &&
                    candidate.medianReprojectionError <= 1.0;
                if (lowDensityButStable && candidate.quality == VisualLocalizationQuality::Rejected)
                    candidate.quality = VisualLocalizationQuality::Marginal;
                const bool fixedScaleTranslation = candidate.quality == VisualLocalizationQuality::Marginal &&
                    !candidate.affineEstimated && candidate.inlierCount >= kMinimumTranslationVotes &&
                    candidate.coveredQuadrants >= 2;
                const bool geometricCandidate = candidate.affineEstimated && candidate.inlierCount >= 4;
                if (geometricCandidate || fixedScaleTranslation) candidates.push_back(std::move(candidate));
            }
        }
        std::sort(candidates.begin(), candidates.end(), BetterCandidate);

        std::vector<VisualLocalizationCandidate> distinct;
        distinct.reserve(candidates.size());
        for (const auto& candidate : candidates) {
            const auto duplicate = std::find_if(distinct.begin(), distinct.end(), [&](const auto& accepted) {
                return accepted.sceneId == candidate.sceneId &&
                    Distance(accepted.mapCenter, candidate.mapCenter) <= kDuplicateCenterDistance;
            });
            if (duplicate == distinct.end()) distinct.push_back(candidate);
        }
        std::sort(distinct.begin(), distinct.end(), BetterCandidate);
        if (distinct.size() > kMaximumCoarseCandidates) distinct.resize(kMaximumCoarseCandidates);

        if (!distinct.empty()) {
            result.quality = distinct.front().quality;
            const bool fixedScaleRunnerUpIsClearlyLessPrecise = distinct.size() > 1 &&
                !distinct.front().affineEstimated && !distinct[1].affineEstimated &&
                distinct[1].medianReprojectionError > distinct.front().medianReprojectionError * 1.5;
            if (distinct.size() > 1 && distinct.front().inlierCount > 0 &&
                (distinct.front().sceneId != distinct[1].sceneId ||
                    Distance(distinct.front().mapCenter, distinct[1].mapCenter) > kDuplicateCenterDistance) &&
                distinct[1].inlierCount * 5 >= distinct.front().inlierCount * 4 &&
                !fixedScaleRunnerUpIsClearlyLessPrecise) {
                result.ambiguous = true;
                result.quality = VisualLocalizationQuality::Rejected;
            }
        }
        result.candidates = std::move(distinct);
        result.verificationMilliseconds = ElapsedMilliseconds(verificationStart);
        result.totalMilliseconds = ElapsedMilliseconds(totalStart);
        return result;
    }

    bool Track(const cv::Size minimapSize, const ImageFeatureData& minimapFeatures,
        int sceneId, const Coordinate& previousCenter, VisualLocalizationCandidate& output,
        double searchRadius = 0.0, const std::function<bool()>& interrupted = {},
        double fixedTerrainScale = 0.0) const {
        const double expectedScale = Scene::MinimapScale(sceneId);
        if (fixedTerrainScale == 0.0) fixedTerrainScale = expectedScale;
        if (searchRadius <= 0.0 && (!std::isfinite(fixedTerrainScale) ||
            fixedTerrainScale < expectedScale * (1.0 - kMaximumScaleDeviation) ||
            fixedTerrainScale > expectedScale * (1.0 + kMaximumScaleDeviation))) return false;
        std::vector<std::uint32_t> rows;
        for (std::uint32_t tileIndex = 0; tileIndex < resources_->visualIndex.tiles.size(); ++tileIndex) {
            const auto& tile = resources_->visualIndex.tiles[tileIndex];
            const int tileSceneId = tileIndex < resources_->baseVisualTileCount ? 1 : tile.sceneId;
            if (tileSceneId != sceneId) continue;
            const double nearestX = std::clamp(previousCenter.x, static_cast<double>(tile.minX), static_cast<double>(tile.maxX));
            const double nearestY = std::clamp(previousCenter.y, static_cast<double>(tile.minY), static_cast<double>(tile.maxY));
            const bool overlapsPreviousTile = previousCenter.x >= tile.minX && previousCenter.x < tile.maxX &&
                previousCenter.y >= tile.minY && previousCenter.y < tile.maxY;
            if (!overlapsPreviousTile && (searchRadius <= 0.0 ||
                std::hypot(previousCenter.x - nearestX, previousCenter.y - nearestY) > searchRadius)) continue;
            rows.insert(rows.end(), resources_->visualIndex.featureRows.begin() + tile.featureRowOffset,
                resources_->visualIndex.featureRows.begin() + tile.featureRowOffset + tile.featureRowCount);
        }
        std::sort(rows.begin(), rows.end());
        rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
        if (rows.empty()) return false;
        output = VerifyRows(rows, minimapFeatures, minimapSize, sceneId, 0.0, {}, interrupted,
            nullptr, searchRadius <= 0.0 ? fixedTerrainScale : 0.0);
        const bool acceptedGeometry = output.inlierCount >= 6 && output.inlierRatio >= 0.20 &&
            output.medianReprojectionError <= 4.0 && output.coveredQuadrants >= 2;
        const bool acceptedTranslation = output.quality == VisualLocalizationQuality::Marginal &&
            !output.affineEstimated && output.inlierCount >= kMinimumTranslationVotes &&
            output.coveredQuadrants >= 2 &&
            output.medianReprojectionError <= kTranslationVoteRadius;
        return (acceptedGeometry || acceptedTranslation) &&
            Distance(output.mapCenter, previousCenter) <= (searchRadius > 0.0 ? searchRadius : 30.0);
    }

private:
    std::vector<std::pair<std::uint32_t, double>> RetrieveTiles(const cv::Mat& descriptors,
        const std::vector<VisualMapHint>* hints = nullptr) const {
        cv::Mat words(descriptors.rows, 1, CV_32S);
        cv::Mat distances(descriptors.rows, 1, CV_32F);
        vocabularyIndex_.knnSearch(descriptors, words, distances, 1, cv::flann::SearchParams(64));
        std::vector<std::uint32_t> counts(MapVisualIndex::WordCount, 0);
        for (int row = 0; row < words.rows; ++row) {
            const int word = words.at<int>(row);
            if (word >= 0 && word < static_cast<int>(counts.size())) ++counts[word];
        }
        std::vector<double> queryWeights(MapVisualIndex::WordCount, 0.0);
        double normSquared = 0.0;
        const double tileCount = static_cast<double>(resources_->visualIndex.tiles.size());
        for (std::uint32_t word = 0; word < MapVisualIndex::WordCount; ++word) {
            if (counts[word] == 0) continue;
            const auto begin = resources_->visualIndex.postingOffsets[word];
            const auto end = resources_->visualIndex.postingOffsets[word + 1];
            const double idf = std::log((tileCount + 1.0) / (static_cast<double>(end - begin) + 1.0)) + 1.0;
            const double tf = static_cast<double>(counts[word]) / descriptors.rows;
            queryWeights[word] = tf * idf;
            normSquared += queryWeights[word] * queryWeights[word];
        }
        if (!(normSquared > 0.0)) return {};
        const double inverseNorm = 1.0 / std::sqrt(normSquared);
        std::vector<double> scores(resources_->visualIndex.tiles.size(), 0.0);
        for (std::uint32_t word = 0; word < MapVisualIndex::WordCount; ++word) {
            const double queryWeight = queryWeights[word] * inverseNorm;
            if (!(queryWeight > 0.0)) continue;
            const auto begin = resources_->visualIndex.postingOffsets[word];
            const auto end = resources_->visualIndex.postingOffsets[word + 1];
            for (std::uint32_t postingIndex = begin; postingIndex < end; ++postingIndex) {
                const auto& posting = resources_->visualIndex.postings[postingIndex];
                scores[posting.tileIndex] += queryWeight * posting.weight;
            }
        }
        std::map<std::tuple<int, int, int>, std::pair<std::uint32_t, double>> groupedScores;
        for (std::uint32_t tileIndex = 0; tileIndex < scores.size(); ++tileIndex) {
            const auto& tile = resources_->visualIndex.tiles[tileIndex];
            const bool baseTile = resources_->baseVisualTileCount > 0 &&
                tileIndex < resources_->baseVisualTileCount;
            const int resultSceneId = baseTile ? Scene::SceneNameToId("World") : tile.sceneId;
            if (hints != nullptr && !TileMatchesHint(tile, resultSceneId, *hints)) continue;
            // Include zero-score copies when choosing the representative so
            // its identity stays stable as query scores change across frames.
            auto [entry, inserted] = groupedScores.try_emplace(
                std::tuple{resultSceneId, tile.gridY, tile.gridX}, std::pair{tileIndex, 0.0});
            auto& group = entry->second;
            group.first = std::min(group.first, tileIndex);
            // Copies of the same region are alternate observations, not
            // independent evidence. Summing makes overlapping packs crowd out
            // the base map before geometric verification can even inspect it.
            group.second = std::max(group.second, scores[tileIndex]);
        }
        std::vector<std::pair<std::uint32_t, double>> grouped;
        grouped.reserve(groupedScores.size());
        for (const auto& [key, value] : groupedScores) if (value.second > 0.0) grouped.push_back(value);
        std::sort(grouped.begin(), grouped.end(), [](const auto& left, const auto& right) {
            if (left.second != right.second) return left.second > right.second;
            return left.first < right.first;
        });
        return grouped;
    }

    MatchedRows MatchRows(std::span<const uint32_t> rows, const ImageFeatureData& query,
        const std::function<bool()>& interrupted) const {
        static const bool profile = [] { char value[8]{}; size_t length = 0;
            return getenv_s(&length, value, sizeof(value), "IMAO_PROFILE_MATCHING") == 0 && value[0] == '1'; }();
        const auto start = std::chrono::steady_clock::now();
        MatchedRows matches;
        std::vector<std::uint32_t> enabledRows;
        if (!resources_->excludedBaseRows.empty()) {
            enabledRows.reserve(rows.size());
            for (const auto row : rows) if (resources_->FeatureRowEnabled(row)) enabledRows.push_back(row);
            rows = enabledRows;
        }
        matches.subset = rowCache_.Get(rows, interrupted);
        const auto prepared = std::chrono::steady_clock::now();
        MatchDescriptorsExactly(matches.subset->imgDescriptors, query.imgDescriptors,
            matches.forward, matches.reverse, interrupted);
        if (profile) std::cerr << "matching-profile rows=" << rows.size() << " query=" << query.imgDescriptors.rows
            << " prepareMs=" << std::chrono::duration<double, std::milli>(prepared - start).count()
            << " matchMs=" << ElapsedMilliseconds(prepared) << '\n';
        return matches;
    }

    std::vector<VisualLocalizationCandidate> VerifyTranslationRows(
        std::span<const std::uint32_t> featureRows, const ImageFeatureData& minimapFeatures,
        const cv::Size minimapSize, int sceneId, double retrievalScore, const std::function<bool()>& interrupted = {},
        const MatchedRows* sharedMatches = nullptr) const {
        if (featureRows.size() < static_cast<std::size_t>(kMinimumTranslationVotes) ||
            minimapFeatures.imgDescriptors.rows < kMinimumTranslationVotes) return {};
        MatchedRows ownedMatches;
        if (!sharedMatches) { ownedMatches = MatchRows(featureRows, minimapFeatures, interrupted); sharedMatches = &ownedMatches; }
        const auto& mapDescriptors = sharedMatches->subset->imgDescriptors;
        const auto& mapKeypoints = sharedMatches->subset->imgKeypoints;
        if (mapDescriptors.rows < kMinimumTranslationVotes) return {};
        const auto& forward = sharedMatches->forward;
        const auto& reverse = sharedMatches->reverse;
        std::vector<TranslationVote> votes;
        votes.reserve(forward.size());
        for (const auto& pair : forward) {
            if (pair.size() < 2 ||
                pair[0].distance >= kTranslationVoteRatioThreshold * pair[1].distance ||
                pair[0].distance >= kTranslationVoteDistanceThreshold) continue;
            const auto& mapPoint = mapKeypoints[static_cast<std::size_t>(pair[0].queryIdx)].pt;
            const auto& minimapPoint = minimapFeatures.imgKeypoints[static_cast<std::size_t>(pair[0].trainIdx)].pt;
            votes.push_back({
                { mapPoint.x - Scene::MinimapScale(sceneId) * (minimapPoint.x - minimapSize.width / 2.0),
                    mapPoint.y - Scene::MinimapScale(sceneId) * (minimapPoint.y - minimapSize.height / 2.0) },
                minimapPoint, pair[0].distance });
        }
        return BuildTranslationVoteCandidates(std::move(votes), minimapSize, sceneId,
            retrievalScore, 0);
    }

    VisualLocalizationCandidate VerifyRows(std::span<const std::uint32_t> featureRows,
        const ImageFeatureData& minimapFeatures, const cv::Size minimapSize, int sceneId,
        double retrievalScore, const std::vector<VisualMapHint>& hints,
        const std::function<bool()>& interrupted = {}, const MatchedRows* sharedMatches = nullptr,
        double fixedTerrainScale = 0.0) const {
        VisualLocalizationCandidate candidate;
        candidate.sceneId = sceneId;
        candidate.retrievalScore = retrievalScore;
        if (featureRows.size() < 4 || minimapFeatures.imgDescriptors.rows < 4) return candidate;

        MatchedRows ownedMatches;
        if (!sharedMatches) { ownedMatches = MatchRows(featureRows, minimapFeatures, interrupted); sharedMatches = &ownedMatches; }
        const auto& mapDescriptors = sharedMatches->subset->imgDescriptors;
        const auto& mapKeypoints = sharedMatches->subset->imgKeypoints;
        if (mapDescriptors.rows < 4) return {};
        const auto& forward = sharedMatches->forward;
        const auto& reverse = sharedMatches->reverse;
        std::vector<cv::DMatch> matches;
        std::vector<TranslationVote> translationVotes;
        const double translationScale = fixedTerrainScale > 0.0 ? fixedTerrainScale : Scene::MinimapScale(sceneId);
        translationVotes.reserve(forward.size());
        for (const auto& pair : forward) {
            // The minimap palette/AA changed in recent game builds.  Relax the
            // descriptor prefilter slightly, then retain the much stronger
            // mutual-neighbour, RANSAC, scale, reprojection and quadrant tests
            // below.  This recovers genuine terrain matches without accepting
            // a one-off visual coincidence as a player position.
            if (pair.size() < 2) continue;
            if (pair[0].distance < kTranslationVoteRatioThreshold * pair[1].distance &&
                pair[0].distance < kTranslationVoteDistanceThreshold) {
                const auto& mapPoint = mapKeypoints[static_cast<std::size_t>(pair[0].queryIdx)].pt;
                const auto& minimapPoint = minimapFeatures.imgKeypoints[static_cast<std::size_t>(pair[0].trainIdx)].pt;
                translationVotes.push_back({
                    { mapPoint.x - translationScale * (minimapPoint.x - minimapSize.width / 2.0),
                        mapPoint.y - translationScale * (minimapPoint.y - minimapSize.height / 2.0) },
                    minimapPoint, pair[0].distance });
            }
            if (pair[0].distance >= kDescriptorRatioThreshold * pair[1].distance ||
                pair[0].distance > kDescriptorDistanceThreshold) continue;
            const auto minimapRow = pair[0].trainIdx;
            if (minimapRow < 0 || minimapRow >= reverse.size() || reverse[minimapRow].empty() ||
                reverse[minimapRow][0].trainIdx != pair[0].queryIdx) continue;
            matches.push_back(pair[0]);
        }
        candidate.mutualMatchCount = static_cast<int>(matches.size());
        const auto translationCandidate = BuildTranslationVoteCandidate(translationVotes, minimapSize,
            sceneId, retrievalScore, candidate.mutualMatchCount, translationScale);
        if (matches.size() < 4) return translationCandidate.inlierCount >= kMinimumTranslationVotes
            ? translationCandidate : candidate;

        std::vector<cv::Point2f> minimapPoints;
        std::vector<cv::Point2f> mapPoints;
        minimapPoints.reserve(matches.size());
        mapPoints.reserve(matches.size());
        for (const auto& match : matches) {
            minimapPoints.push_back(minimapFeatures.imgKeypoints[match.trainIdx].pt);
            mapPoints.push_back(mapKeypoints[match.queryIdx].pt);
        }
        cv::Mat inlierMask;
        cv::Mat transform = cv::estimateAffinePartial2D(minimapPoints, mapPoints, inlierMask,
            cv::RANSAC, 3.0, 2000, 0.99, 10);
        if (transform.empty() || transform.rows != 2 || transform.cols != 3) return translationCandidate.inlierCount >= kMinimumTranslationVotes
            ? translationCandidate : candidate;
        candidate.affineEstimated = true;

        const double a = transform.at<double>(0, 0);
        const double b = transform.at<double>(1, 0);
        const double scale = std::hypot(a, b);
        candidate.scale = scale;
        candidate.rotationDegrees = std::atan2(b, a) * 180.0 / CV_PI;
        if (!std::isfinite(scale) || scale < Scene::MinimapScale(sceneId) * (1.0 - kMaximumScaleDeviation) ||
            scale > Scene::MinimapScale(sceneId) * (1.0 + kMaximumScaleDeviation)) return translationCandidate.inlierCount >= kMinimumTranslationVotes
            ? translationCandidate : candidate;
        candidate.scaleWithinExpectedRange = true;

        if (fixedTerrainScale > 0.0) {
            // Keep similarity fitting for descriptor consensus and global /
            // recovery diagnostics. During gameplay, changing terrain support
            // must not turn camera-cone changes into fictitious map zoom or
            // rotation, which also displaces the extrapolated player center.
            const auto tracking = FitMinimapTrackingTranslation(minimapPoints, mapPoints,
                minimapSize, inlierMask, fixedTerrainScale);
            if (!tracking.empty()) {
                transform = tracking;
                candidate.scale = fixedTerrainScale;
                candidate.rotationDegrees = 0.0;
            }
            else {
                // The fixed scale is a tracking-stability rule, not a correctness rule. When it
                // cannot explain these matches at all, the similarity fit above already has to
                // hold support to have reached this point - measured inside a layered map: the
                // fixed fit found no consensus while the similarity fit kept 20 inliers at a
                // scale 1.9% off the expected one. Throwing that away and dropping to a
                // three-vote translation is what made a cold start inside a cave unpublishable:
                // reacquisition requires affine support, and every candidate came back
                // translation-only. North-up is still enforced, because the minimap is north-up
                // by construction.
                candidate.rotationDegrees = 0.0;
            }
        }

        const cv::Point2d center(minimapSize.width / 2.0, minimapSize.height / 2.0);
        candidate.mapCenter.x = transform.at<double>(0, 0) * center.x +
            transform.at<double>(0, 1) * center.y + transform.at<double>(0, 2);
        candidate.mapCenter.y = transform.at<double>(1, 0) * center.x +
            transform.at<double>(1, 1) * center.y + transform.at<double>(1, 2);

        std::vector<double> errors;
        std::array<bool, 4> quadrants{};
        for (int index = 0; index < inlierMask.rows; ++index) {
            if (inlierMask.at<std::uint8_t>(index) == 0) continue;
            ++candidate.inlierCount;
            const auto& source = minimapPoints[index];
            const auto& expected = mapPoints[index];
            const double projectedX = transform.at<double>(0, 0) * source.x +
                transform.at<double>(0, 1) * source.y + transform.at<double>(0, 2);
            const double projectedY = transform.at<double>(1, 0) * source.x +
                transform.at<double>(1, 1) * source.y + transform.at<double>(1, 2);
            errors.push_back(std::hypot(projectedX - expected.x, projectedY - expected.y));
            const int quadrant = (source.x >= center.x ? 1 : 0) + (source.y >= center.y ? 2 : 0);
            quadrants[quadrant] = true;
        }
        if (errors.empty()) return translationCandidate.inlierCount >= kMinimumTranslationVotes
            ? translationCandidate : candidate;
        std::sort(errors.begin(), errors.end());
        candidate.medianReprojectionError = errors[errors.size() / 2];
        candidate.inlierRatio = static_cast<double>(candidate.inlierCount) / matches.size();
        candidate.coveredQuadrants = static_cast<int>(std::count(quadrants.begin(), quadrants.end(), true));
        if (candidate.inlierCount >= 12 && candidate.inlierRatio >= 0.35 &&
            candidate.medianReprojectionError <= 3.0 && candidate.coveredQuadrants >= 3) {
            candidate.quality = VisualLocalizationQuality::Strong;
        }
        else if (candidate.inlierCount >= 8 && candidate.inlierRatio >= 0.25 &&
            candidate.medianReprojectionError <= 4.5 && candidate.coveredQuadrants >= 2) {
            candidate.quality = VisualLocalizationQuality::Marginal;
        }
        if (candidate.quality == VisualLocalizationQuality::Rejected &&
            translationCandidate.inlierCount >= kMinimumTranslationVotes) return translationCandidate;
        for (const auto& hint : hints) {
            if (hint.sceneId == sceneId && Distance(candidate.mapCenter, hint.mapCoordinate) <= kOcrHintDistance) {
                candidate.ocrHintMatched = true;
                break;
            }
        }
        return candidate;
    }

    std::shared_ptr<const RuntimeFeatureResources> resources_;
    FeatureRowCache rowCache_;
    mutable cv::flann::Index vocabularyIndex_;
};

class VisualRuntime {
public:
    bool Initialize(std::shared_ptr<const RuntimeFeatureResources> resources, std::string& error) {
        std::scoped_lock lock(mutex_);
        if (ready_) return true;
        if (!resources || !resources->visualIndexReady) {
            error = "runtime visual index is unavailable";
            return false;
        }
        try {
            resources_ = std::move(resources);
            engine_ = std::make_unique<VisualLocalizationEngine>(resources_);
            worker_ = std::jthread([this](std::stop_token token) { Worker(token); });
            ThreadPriority::MakeBackground(worker_);
            ready_ = true;
            error.clear();
            return true;
        }
        catch (const std::exception& exception) {
            resources_.reset();
            engine_.reset();
            error = exception.what();
            return false;
        }
    }

    void Shutdown() {
        std::jthread worker;
        {
            std::scoped_lock lock(mutex_);
            ready_ = false;
            ++epoch_;
            ++recoveryEpoch_;
            if (worker_.joinable()) worker_.request_stop();
            worker = std::move(worker_);
            condition_.notify_all();
        }
        if (worker.joinable()) worker.join();
        std::scoped_lock lock(mutex_);
        request_.reset();
        result_.reset();
        engine_.reset();
        resources_.reset();
        ready_ = false;
    }

    bool Ready() const {
        std::scoped_lock lock(mutex_);
        return ready_;
    }

    bool Submit(VisualLocalizationRequest request) {
        std::scoped_lock lock(mutex_);
        if (!ready_) return false;
        ++epoch_;
        result_.reset();
        request_ = std::move(request);
        condition_.notify_one();
        return true;
    }

    void CancelPending() {
        std::scoped_lock lock(mutex_);
        ++epoch_;
        ++recoveryEpoch_;
        request_.reset(); result_.reset();
    }

    bool Take(VisualLocalizationResult& result) {
        std::scoped_lock lock(mutex_);
        if (!result_) return false;
        result = std::move(*result_);
        result_.reset();
        return true;
    }

    bool Track(const cv::Mat& minimap, const ImageFeatureData& features, int sceneId,
        const Coordinate& previous, VisualLocalizationCandidate& result, double searchRadius = 0.0,
        double fixedTerrainScale = 0.0) {
        std::scoped_lock lock(mutex_);
        if (!ready_ || !engine_) return false;
        return engine_->Track(minimap.size(), features, sceneId, previous, result, searchRadius,
            {}, fixedTerrainScale);
    }

private:
    void Worker(std::stop_token token) {
        for (;;) {
            VisualLocalizationRequest request;
            VisualLocalizationEngine* engine = nullptr;
            uint64_t epoch = 0;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [&] { return token.stop_requested() || request_.has_value(); });
                if (token.stop_requested()) return;
                request = std::move(*request_);
                request_.reset();
                engine = engine_.get();
                epoch = epoch_.load();
            }

            // A bad or partially updated capture must only reject this request.
            // Letting an exception escape a std::jthread invokes std::terminate
            // and closes the whole WinUI application.
            VisualLocalizationResult result;
            result.sessionId = request.sessionId;
            result.uiGeneration = request.uiGeneration;
            result.frameId = request.frameId;
            result.requestId = request.requestId;
            result.hintVersion = request.hintVersion;
            result.quality = VisualLocalizationQuality::Rejected;
            const auto cancellation = recoveryEpoch_.load();
            const bool restart = recoverySession_ != request.sessionId || recoveryGeneration_ != request.uiGeneration ||
                recoveryHintVersion_ != request.hintVersion || recoveryOcrBound_ != request.requireOcrHint ||
                appliedRecoveryEpoch_ != cancellation;
            auto coverage = restart ? CoarseSearchCoverage{} : coverage_;
            auto phase = restart ? std::size_t{0} : recoveryPhase_;
            auto hint = restart ? std::optional<VisualLocalizationCandidate>{} : candidateHint_;
            const auto searchStart = std::chrono::steady_clock::now();
            try {
                if (engine != nullptr) {
                    static constexpr double angles[] = {0.0, -45.0, 45.0, -90.0, 90.0, -135.0, 135.0, 180.0};
                    const auto deadline = searchStart + std::chrono::milliseconds(350);
                    const auto interrupted = [&] {
                        return token.stop_requested() || epoch_.load() != epoch || std::chrono::steady_clock::now() >= deadline;
                    };
                    // A global match remains only a hint. Use this new frame's
                    // geometry to confirm it without relying on its coarse rank
                    // remaining high enough on the next capture.
                    VisualLocalizationCandidate current;
                    if (!request.requireOcrHint && hint && request.frameId > candidateHintFrame_ &&
                        searchStart - candidateHintAt_ <= std::chrono::seconds(2) &&
                        engine->Track(request.normalizedMinimap.size(), request.minimapFeatures,
                            hint->sceneId, hint->mapCenter, current, 64.0, interrupted) &&
                        current.quality != VisualLocalizationQuality::Rejected &&
                        HasReacquisitionSupport(current.affineEstimated, current.inlierCount,
                            current.inlierRatio, current.coveredQuadrants)) {
                        result.quality = current.quality;
                        result.candidates.push_back(current);
                        result.usedCandidateHint = true;
                        result.bestMutualMatchCount = current.mutualMatchCount;
                        result.bestInlierCount = current.inlierCount;
                        result.bestObservedScale = current.scale;
                        result.bestAffineEstimated = current.affineEstimated;
                        result.bestScaleWithinExpectedRange = current.scaleWithinExpectedRange;
                    }
                    else result = engine->Locate(request, angles[phase], interrupted, &coverage);
                    const bool supported = !result.searchIncomplete && !result.ambiguous &&
                        result.quality != VisualLocalizationQuality::Rejected && !result.candidates.empty() &&
                        HasReacquisitionSupport(result.candidates.front().affineEstimated,
                            result.candidates.front().inlierCount, result.candidates.front().inlierRatio,
                            result.candidates.front().coveredQuadrants);
                    if (supported) {
                        hint = result.candidates.front();
                        hint->ocrHintMatched = false;
                        phase = 0;
                        // One isolated match must not restart the entire
                        // search if its next-frame validation later fails.
                        if (result.usedCandidateHint) coverage.Reset();
                    }
                    else {
                        hint.reset();
                        phase = (phase + 1) % std::size(angles);
                    }
                }
                else Diagnostics::Record("visual-localization-worker", "request rejected: engine unavailable");
            }
            catch (const SearchInterrupted&) {
                result.quality = VisualLocalizationQuality::Rejected;
                result.searchIncomplete = true;
                result.candidates.clear();
                hint.reset();
            }
            catch (const cv::Exception& exception) {
                hint.reset(); coverage.Reset();
                Diagnostics::Record("visual-localization-worker", std::string("OpenCV exception: ") + exception.what());
            }
            catch (const std::exception& exception) {
                hint.reset(); coverage.Reset();
                Diagnostics::Record("visual-localization-worker", std::string("exception: ") + exception.what());
            }
            catch (...) {
                hint.reset(); coverage.Reset();
                Diagnostics::Record("visual-localization-worker", "unknown exception");
            }
            {
                std::scoped_lock lock(mutex_);
                if (epoch_.load() == epoch && !token.stop_requested()) {
                    // Cancelled/replaced work cannot consume another request's
                    // coverage or seed its next-frame candidate.
                    coverage_ = std::move(coverage);
                    recoveryPhase_ = phase;
                    recoverySession_ = request.sessionId; recoveryGeneration_ = request.uiGeneration;
                    recoveryHintVersion_ = request.hintVersion; recoveryOcrBound_ = request.requireOcrHint;
                    appliedRecoveryEpoch_ = cancellation;
                    candidateHint_ = std::move(hint);
                    if (candidateHint_) { candidateHintAt_ = searchStart; candidateHintFrame_ = request.frameId; }
                    result.totalMilliseconds = ElapsedMilliseconds(searchStart);
                    result_ = std::move(result);
                }
            }
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::jthread worker_;
    bool ready_ = false;
    std::atomic_uint64_t epoch_{0};
    std::atomic_uint64_t recoveryEpoch_{0};
    uint64_t appliedRecoveryEpoch_ = 0, recoveryHintVersion_ = 0;
    bool recoveryOcrBound_ = false;
    CoarseSearchCoverage coverage_;
    std::optional<VisualLocalizationCandidate> candidateHint_;
    std::chrono::steady_clock::time_point candidateHintAt_{};
    uint64_t candidateHintFrame_ = 0;
    size_t recoveryPhase_ = 0;
    uint64_t recoverySession_ = 0, recoveryGeneration_ = 0;
    std::shared_ptr<const RuntimeFeatureResources> resources_;
    std::unique_ptr<VisualLocalizationEngine> engine_;
    std::optional<VisualLocalizationRequest> request_;
    std::optional<VisualLocalizationResult> result_;
};

VisualRuntime& Runtime() {
    static VisualRuntime runtime;
    return runtime;
}
}

bool GlobalVisualLocalizer::Initialize(std::shared_ptr<const RuntimeFeatureResources> resources, std::string& error) {
    return Runtime().Initialize(std::move(resources), error);
}

void GlobalVisualLocalizer::Shutdown() {
    Runtime().Shutdown();
}

bool GlobalVisualLocalizer::IsReady() {
    return Runtime().Ready();
}

bool GlobalVisualLocalizer::PrepareMinimap(const cv::Mat& minimap, cv::Mat& normalized,
    ImageFeatureData& features, const cv::Mat* trustedReference,
    MinimapFeatureDiagnostics* diagnostics, double surfThreshold) {
    if (minimap.empty()) return false;
    cv::resize(minimap, normalized, cv::Size(184, 184), 0.0, 0.0, cv::INTER_AREA);
    return ExtractPreparedMinimapFeatures(normalized, features, trustedReference, diagnostics, surfThreshold);
}

bool GlobalVisualLocalizer::Submit(VisualLocalizationRequest request) {
    return Runtime().Submit(std::move(request));
}

bool GlobalVisualLocalizer::TryTakeLatestResult(VisualLocalizationResult& result) {
    return Runtime().Take(result);
}

VisualLocalizationResult GlobalVisualLocalizer::LocateForDiagnostics(
    std::shared_ptr<const RuntimeFeatureResources> resources, const VisualLocalizationRequest& request) {
    if (!resources || !resources->visualIndexReady) return {};
    VisualLocalizationEngine engine(std::move(resources));
    return engine.Locate(request);
}

bool GlobalVisualLocalizer::TrackLocal(const cv::Mat& normalizedMinimap,
    const ImageFeatureData& minimapFeatures, int sceneId, const Coordinate& previousMapCenter,
    VisualLocalizationCandidate& result, double fixedTerrainScale) {
    return Runtime().Track(normalizedMinimap, minimapFeatures, sceneId, previousMapCenter, result,
        0.0, fixedTerrainScale);
}

bool GlobalVisualLocalizer::TrackNearby(const cv::Mat& normalizedMinimap,
    const ImageFeatureData& minimapFeatures, int sceneId, const Coordinate& previousMapCenter,
    double searchRadius, VisualLocalizationCandidate& result) {
    return Runtime().Track(normalizedMinimap, minimapFeatures, sceneId, previousMapCenter, result,
        searchRadius);
}

std::string_view GlobalVisualLocalizer::QualityName(VisualLocalizationQuality quality) {
    switch (quality) {
    case VisualLocalizationQuality::Rejected: return "Rejected";
    case VisualLocalizationQuality::Marginal: return "Marginal";
    case VisualLocalizationQuality::Strong: return "Strong";
    }
    return "Unknown";
}

void GlobalVisualLocalizer::CancelPending() { Runtime().CancelPending(); }
