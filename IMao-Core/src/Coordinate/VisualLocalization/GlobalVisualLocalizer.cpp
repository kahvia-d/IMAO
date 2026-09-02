#include "GlobalVisualLocalizer.h"

#include "../../Diagnostics/Diagnostics.h"

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

bool ExtractPreparedMinimapFeatures(const cv::Mat& normalized, ImageFeatureData& features,
    double surfThreshold = 10.0) {
    if (normalized.empty()) return false;
    cv::Mat gray;
    if (normalized.channels() == 1) gray = normalized;
    else if (normalized.channels() == 4) cv::cvtColor(normalized, gray, cv::COLOR_BGRA2GRAY);
    else cv::cvtColor(normalized, gray, cv::COLOR_BGR2GRAY);
    cv::Mat mask(gray.size(), CV_8UC1, cv::Scalar(0));
    const cv::Point center(gray.cols / 2, gray.rows / 2);
    cv::circle(mask, center, 87, cv::Scalar(255), cv::FILLED);
    cv::circle(mask, center, 16, cv::Scalar(0), cv::FILLED);
    auto surf = cv::xfeatures2d::SURF::create(surfThreshold, 8, 4, true, true);
    std::vector<cv::KeyPoint> keypoints;
    cv::Mat descriptors;
    surf->detectAndCompute(gray, mask, keypoints, descriptors);
    features = ImageFeatureData(keypoints, descriptors);
    return !features.imgDescriptors.empty();
}

double ElapsedMilliseconds(const std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

double Distance(const Coordinate& left, const Coordinate& right) {
    return std::hypot(left.x - right.x, left.y - right.y);
}

int QualityRank(VisualLocalizationQuality quality) {
    return static_cast<int>(quality);
}

bool BetterCandidate(const VisualLocalizationCandidate& left, const VisualLocalizationCandidate& right) {
    if (QualityRank(left.quality) != QualityRank(right.quality)) {
        return QualityRank(left.quality) > QualityRank(right.quality);
    }
    if (left.ocrHintMatched != right.ocrHintMatched) return left.ocrHintMatched;
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
public:
    explicit VisualLocalizationEngine(std::shared_ptr<const RuntimeFeatureResources> resources)
        : resources_(std::move(resources)), vocabularyIndex_(
            resources_->visualIndex.vocabulary, cv::flann::KDTreeIndexParams(4)) {
    }

    VisualLocalizationResult Locate(const VisualLocalizationRequest& request) const {
        const auto overallStart = std::chrono::steady_clock::now();
        auto baseline = LocateOnce(request);
        if (baseline.quality != VisualLocalizationQuality::Rejected || request.normalizedMinimap.empty()) {
            return baseline;
        }

        static constexpr std::array<double, 7> corrections = {
            -45.0, 45.0, -90.0, 90.0, -135.0, 135.0, 180.0
        };
        double bestScore = baseline.bestRetrievalScore;
        double rotationCoarseMilliseconds = 0.0;
        struct RotationHypothesis {
            double score = 0.0;
            double correction = 0.0;
            VisualLocalizationRequest request;
        };
        std::vector<RotationHypothesis> hypotheses;
        for (const double correction : corrections) {
            VisualLocalizationRequest rotated = request;
            const cv::Point2f center(request.normalizedMinimap.cols / 2.0f,
                request.normalizedMinimap.rows / 2.0f);
            const auto matrix = cv::getRotationMatrix2D(center, correction, 1.0);
            cv::warpAffine(request.normalizedMinimap, rotated.normalizedMinimap, matrix,
                request.normalizedMinimap.size(), cv::INTER_LINEAR, cv::BORDER_REFLECT_101);
            if (!ExtractPreparedMinimapFeatures(rotated.normalizedMinimap, rotated.minimapFeatures)) continue;
            const auto coarseStart = std::chrono::steady_clock::now();
            const auto tiles = RetrieveTiles(rotated.minimapFeatures.imgDescriptors);
            rotationCoarseMilliseconds += ElapsedMilliseconds(coarseStart);
            const double score = tiles.empty() ? 0.0 : tiles.front().second;
            hypotheses.push_back({ score, correction, rotated });
            if (score >= 0.28 && score >= baseline.bestRetrievalScore * 1.35) {
                auto recovered = LocateOnce(rotated);
                if (recovered.quality != VisualLocalizationQuality::Rejected) {
                    for (auto& candidate : recovered.candidates) {
                        candidate.rotationDegrees -= correction;
                        while (candidate.rotationDegrees > 180.0) candidate.rotationDegrees -= 360.0;
                        while (candidate.rotationDegrees <= -180.0) candidate.rotationDegrees += 360.0;
                    }
                    recovered.coarseMilliseconds += baseline.coarseMilliseconds + rotationCoarseMilliseconds;
                    recovered.verificationMilliseconds += baseline.verificationMilliseconds;
                    recovered.totalMilliseconds = ElapsedMilliseconds(overallStart);
                    return recovered;
                }
                baseline.verificationMilliseconds += recovered.verificationMilliseconds;
            }
            if (score > bestScore) {
                bestScore = score;
            }
        }

        std::sort(hypotheses.begin(), hypotheses.end(), [](const auto& left, const auto& right) {
            if (left.score != right.score) return left.score > right.score;
            return left.correction < right.correction;
        });
        int verifiedHypotheses = 0;
        for (const auto& hypothesis : hypotheses) {
            if (verifiedHypotheses >= 3 || hypothesis.score < 0.15) break;
            ++verifiedHypotheses;
            auto recovered = LocateOnce(hypothesis.request);
            if (recovered.quality != VisualLocalizationQuality::Rejected) {
                for (auto& candidate : recovered.candidates) {
                    candidate.rotationDegrees -= hypothesis.correction;
                    while (candidate.rotationDegrees > 180.0) candidate.rotationDegrees -= 360.0;
                    while (candidate.rotationDegrees <= -180.0) candidate.rotationDegrees += 360.0;
                }
                recovered.coarseMilliseconds += baseline.coarseMilliseconds + rotationCoarseMilliseconds;
                recovered.verificationMilliseconds += baseline.verificationMilliseconds;
                recovered.totalMilliseconds = ElapsedMilliseconds(overallStart);
                return recovered;
            }
            baseline.verificationMilliseconds += recovered.verificationMilliseconds;
        }
        baseline.bestRetrievalScore = std::max(baseline.bestRetrievalScore, bestScore);
        baseline.coarseMilliseconds += rotationCoarseMilliseconds;
        baseline.totalMilliseconds = ElapsedMilliseconds(overallStart);
        return baseline;
    }

    VisualLocalizationResult LocateOnce(const VisualLocalizationRequest& request) const {
        VisualLocalizationResult result;
        result.sessionId = request.sessionId;
        result.uiGeneration = request.uiGeneration;
        result.frameId = request.frameId;
        const auto totalStart = std::chrono::steady_clock::now();
        if (request.minimapFeatures.imgDescriptors.empty() ||
            request.minimapFeatures.imgDescriptors.type() != CV_32FC1 ||
            request.minimapFeatures.imgDescriptors.cols != static_cast<int>(MapVisualIndex::DescriptorColumns)) {
            result.totalMilliseconds = ElapsedMilliseconds(totalStart);
            return result;
        }

        const auto coarseStart = std::chrono::steady_clock::now();
        const auto coarseCandidates = RetrieveTiles(request.minimapFeatures.imgDescriptors);
        if (!coarseCandidates.empty()) result.bestRetrievalScore = coarseCandidates.front().second;
        result.coarseMilliseconds = ElapsedMilliseconds(coarseStart);

        const auto verificationStart = std::chrono::steady_clock::now();
        std::vector<VisualLocalizationCandidate> candidates;
        candidates.reserve(coarseCandidates.size());
        for (const auto& [tileIndex, score] : coarseCandidates) {
            const auto& tile = resources_->visualIndex.tiles[tileIndex];
            std::vector<std::uint32_t> rows;
            for (const auto& compatible : resources_->visualIndex.tiles) {
                if (compatible.sceneId != tile.sceneId || compatible.gridX != tile.gridX ||
                    compatible.gridY != tile.gridY) continue;
                rows.insert(rows.end(),
                    resources_->visualIndex.featureRows.begin() + compatible.featureRowOffset,
                    resources_->visualIndex.featureRows.begin() + compatible.featureRowOffset +
                        compatible.featureRowCount);
            }
            std::sort(rows.begin(), rows.end());
            rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
            // The original map feature set only contains the state-8 World map.
            // Its tiles were historically partitioned by nearest scene origin to
            // improve retrieval, which puts Black Shores near Tethys internally.
            // Keep that partitioning for matching, but expose every base match as
            // World so downstream item filtering loads the correct data.
            const bool baseTile = resources_->baseVisualTileCount > 0 &&
                tileIndex < resources_->baseVisualTileCount;
            const int resultSceneId = baseTile ? Scene::SceneNameToId("World") : tile.sceneId;
            VisualLocalizationCandidate candidate = VerifyRows(rows, request.minimapFeatures,
                request.normalizedMinimap.size(), resultSceneId, score, request.ocrHints);
            if (candidate.inlierCount >= 4) candidates.push_back(candidate);
        }

        // Public map additions can contain visual details that were absent from
        // the historical base map used to train the retrieval vocabulary.  In
        // that case the coarse search may not rank the correct World tile high
        // enough to reach verification at all.  When it found no geometric
        // candidate, make one bounded exact pass over optional World features.
        // This remains image-only: it does not use the game's coordinate text.
        if (candidates.empty() && !resources_->kuroVisualShards.empty()) {
            const int worldSceneId = Scene::SceneNameToId("World");
            for (const auto& shard : resources_->kuroVisualShards) {
                if (shard.sceneId != worldSceneId || shard.tileCount == 0 ||
                    shard.firstTile > resources_->visualIndex.tiles.size() ||
                    shard.tileCount > resources_->visualIndex.tiles.size() - shard.firstTile) continue;
                std::vector<std::uint32_t> worldRows;
                for (std::uint32_t tileIndex = shard.firstTile;
                    tileIndex < shard.firstTile + shard.tileCount; ++tileIndex) {
                    const auto& tile = resources_->visualIndex.tiles[tileIndex];
                    worldRows.insert(worldRows.end(),
                        resources_->visualIndex.featureRows.begin() + tile.featureRowOffset,
                        resources_->visualIndex.featureRows.begin() + tile.featureRowOffset +
                            tile.featureRowCount);
                }
                std::sort(worldRows.begin(), worldRows.end());
                worldRows.erase(std::unique(worldRows.begin(), worldRows.end()), worldRows.end());
                if (worldRows.size() < 4) continue;
                VisualLocalizationCandidate candidate = VerifyRows(worldRows, request.minimapFeatures,
                    request.normalizedMinimap.size(), worldSceneId, 0.0, request.ocrHints);
                // A newly added public-map area can have fewer retained SURF
                // features than the legacy map.  Four mutually consistent
                // matches are useful only in this bounded fallback, and only
                // when their geometry is exceptionally tight.  Mark it as
                // marginal so callers still require a second frame before
                // treating it as a fully confirmed position.
                const bool lowDensityButStable = candidate.inlierCount >= 4 &&
                    candidate.inlierRatio >= 0.25 && candidate.coveredQuadrants >= 2 &&
                    candidate.medianReprojectionError <= 1.0;
                if (lowDensityButStable) candidate.quality = VisualLocalizationQuality::Marginal;
                if (candidate.inlierCount >= 4) candidates.push_back(std::move(candidate));
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
            if (distinct.size() > 1 && distinct.front().inlierCount > 0 &&
                Distance(distinct.front().mapCenter, distinct[1].mapCenter) > kDuplicateCenterDistance &&
                distinct[1].inlierCount * 5 >= distinct.front().inlierCount * 4) {
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
        double searchRadius = 0.0) const {
        std::vector<std::uint32_t> rows;
        for (std::uint32_t tileIndex = 0; tileIndex < resources_->visualIndex.tiles.size(); ++tileIndex) {
            const auto& tile = resources_->visualIndex.tiles[tileIndex];
            const bool baseWorldTile = sceneId == Scene::SceneNameToId("World") &&
                resources_->baseVisualTileCount > 0 && tileIndex < resources_->baseVisualTileCount;
            if (!baseWorldTile && tile.sceneId != sceneId) continue;
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
        output = VerifyRows(rows, minimapFeatures, minimapSize, sceneId, 0.0, {});
        return output.inlierCount >= 6 && output.inlierRatio >= 0.20 &&
            output.medianReprojectionError <= 4.0 && output.coveredQuadrants >= 2 &&
            Distance(output.mapCenter, previousCenter) <= (searchRadius > 0.0 ? searchRadius : 30.0);
    }

private:
    std::vector<std::pair<std::uint32_t, double>> RetrieveTiles(const cv::Mat& descriptors) const {
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
            if (!(scores[tileIndex] > 0.0)) continue;
            const auto& tile = resources_->visualIndex.tiles[tileIndex];
            auto& group = groupedScores[{ tile.sceneId, tile.gridY, tile.gridX }];
            if (group.second == 0.0 || tileIndex < group.first) group.first = tileIndex;
            group.second += scores[tileIndex];
        }
        std::vector<std::pair<std::uint32_t, double>> grouped;
        grouped.reserve(groupedScores.size());
        for (const auto& [key, value] : groupedScores) grouped.push_back(value);
        const auto keep = std::min(grouped.size(), kMaximumCoarseCandidates);
        std::partial_sort(grouped.begin(), grouped.begin() + keep, grouped.end(), [](const auto& left, const auto& right) {
            if (left.second != right.second) return left.second > right.second;
            return left.first < right.first;
        });
        std::vector<std::pair<std::uint32_t, double>> output;
        output.insert(output.end(), grouped.begin(), grouped.begin() + keep);
        return output;
    }

    VisualLocalizationCandidate VerifyRows(std::span<const std::uint32_t> featureRows,
        const ImageFeatureData& minimapFeatures, const cv::Size minimapSize, int sceneId,
        double retrievalScore, const std::vector<VisualMapHint>& hints) const {
        VisualLocalizationCandidate candidate;
        candidate.sceneId = sceneId;
        candidate.retrievalScore = retrievalScore;
        if (featureRows.size() < 4 || minimapFeatures.imgDescriptors.rows < 4) return candidate;

        cv::Mat mapDescriptors(static_cast<int>(featureRows.size()),
            resources_->map.imgDescriptors.cols, CV_32FC1);
        std::vector<cv::KeyPoint> mapKeypoints;
        mapKeypoints.reserve(featureRows.size());
        for (std::size_t index = 0; index < featureRows.size(); ++index) {
            const auto row = featureRows[index];
            if (row >= resources_->map.imgKeypoints.size()) return candidate;
            resources_->map.imgDescriptors.row(static_cast<int>(row)).copyTo(mapDescriptors.row(static_cast<int>(index)));
            mapKeypoints.push_back(resources_->map.imgKeypoints[row]);
        }

        cv::BFMatcher matcher(cv::NORM_L2, false);
        std::vector<std::vector<cv::DMatch>> forward;
        std::vector<std::vector<cv::DMatch>> reverse;
        matcher.knnMatch(mapDescriptors, minimapFeatures.imgDescriptors, forward, 2);
        matcher.knnMatch(minimapFeatures.imgDescriptors, mapDescriptors, reverse, 1);
        std::vector<cv::DMatch> matches;
        for (const auto& pair : forward) {
            if (pair.size() < 2 || pair[0].distance >= 0.65f * pair[1].distance || pair[0].distance > 0.5f) continue;
            const auto minimapRow = pair[0].trainIdx;
            if (minimapRow < 0 || minimapRow >= reverse.size() || reverse[minimapRow].empty() ||
                reverse[minimapRow][0].trainIdx != pair[0].queryIdx) continue;
            matches.push_back(pair[0]);
        }
        if (matches.size() < 4) return candidate;

        std::vector<cv::Point2f> minimapPoints;
        std::vector<cv::Point2f> mapPoints;
        minimapPoints.reserve(matches.size());
        mapPoints.reserve(matches.size());
        for (const auto& match : matches) {
            minimapPoints.push_back(minimapFeatures.imgKeypoints[match.trainIdx].pt);
            mapPoints.push_back(mapKeypoints[match.queryIdx].pt);
        }
        cv::Mat inlierMask;
        const cv::Mat transform = cv::estimateAffinePartial2D(minimapPoints, mapPoints, inlierMask,
            cv::RANSAC, 3.0, 2000, 0.99, 10);
        if (transform.empty() || transform.rows != 2 || transform.cols != 3) return candidate;

        const double a = transform.at<double>(0, 0);
        const double b = transform.at<double>(1, 0);
        const double scale = std::hypot(a, b);
        candidate.scale = scale;
        candidate.rotationDegrees = std::atan2(b, a) * 180.0 / CV_PI;
        if (!std::isfinite(scale) || scale < kExpectedScale * (1.0 - kMaximumScaleDeviation) ||
            scale > kExpectedScale * (1.0 + kMaximumScaleDeviation)) return candidate;

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
        if (errors.empty()) return candidate;
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
        for (const auto& hint : hints) {
            if (hint.sceneId == sceneId && Distance(candidate.mapCenter, hint.mapCoordinate) <= kOcrHintDistance) {
                candidate.ocrHintMatched = true;
                break;
            }
        }
        return candidate;
    }

    std::shared_ptr<const RuntimeFeatureResources> resources_;
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
        request_ = std::move(request);
        condition_.notify_one();
        return true;
    }

    bool Take(VisualLocalizationResult& result) {
        std::scoped_lock lock(mutex_);
        if (!result_) return false;
        result = std::move(*result_);
        result_.reset();
        return true;
    }

    bool Track(const cv::Mat& minimap, const ImageFeatureData& features, int sceneId,
        const Coordinate& previous, VisualLocalizationCandidate& result, double searchRadius = 0.0) {
        std::scoped_lock lock(mutex_);
        if (!ready_ || !engine_) return false;
        return engine_->Track(minimap.size(), features, sceneId, previous, result, searchRadius);
    }

private:
    void Worker(std::stop_token token) {
        for (;;) {
            VisualLocalizationRequest request;
            VisualLocalizationEngine* engine = nullptr;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [&] { return token.stop_requested() || request_.has_value(); });
                if (token.stop_requested()) return;
                request = std::move(*request_);
                request_.reset();
                engine = engine_.get();
            }

            // A bad or partially updated capture must only reject this request.
            // Letting an exception escape a std::jthread invokes std::terminate
            // and closes the whole WinUI application.
            VisualLocalizationResult result;
            result.sessionId = request.sessionId;
            result.uiGeneration = request.uiGeneration;
            result.frameId = request.frameId;
            result.quality = VisualLocalizationQuality::Rejected;
            try {
                if (engine != nullptr) result = engine->Locate(request);
                else Diagnostics::Record("visual-localization-worker", "request rejected: engine unavailable");
            }
            catch (const cv::Exception& exception) {
                Diagnostics::Record("visual-localization-worker", std::string("OpenCV exception: ") + exception.what());
            }
            catch (const std::exception& exception) {
                Diagnostics::Record("visual-localization-worker", std::string("exception: ") + exception.what());
            }
            catch (...) {
                Diagnostics::Record("visual-localization-worker", "unknown exception");
            }
            {
                std::scoped_lock lock(mutex_);
                result_ = std::move(result);
            }
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::jthread worker_;
    bool ready_ = false;
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
    ImageFeatureData& features, double surfThreshold) {
    if (minimap.empty()) return false;
    cv::resize(minimap, normalized, cv::Size(184, 184), 0.0, 0.0, cv::INTER_AREA);
    return ExtractPreparedMinimapFeatures(normalized, features, surfThreshold);
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
    VisualLocalizationCandidate& result) {
    return Runtime().Track(normalizedMinimap, minimapFeatures, sceneId, previousMapCenter, result);
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
