#include <atomic>
#include <functional>
#include "../Feature/Match/ExactDescriptorMatcher.h"
#include "MapViewportLocalizer.h"
#include "MapViewportGeometry.h"
#include "MapViewportCandidates.h"
#include "MapViewportMatch.h"
#include "../Feature/Match/UniqueMapFeatures.h"
#include "../Feature/VisualIndex/VisualIndexRetrieval.h"
#include "../Runtime/ThreadPriority.h"
#include "../Diagnostics/Diagnostics.h"

#include <opencv2/calib3d.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/xfeatures2d.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
// Row and candidate selection lives in one shared header so the offline comparison of the two matchers
// drives the same code this file ships. Bringing the names in here keeps every call site unchanged.
using namespace MapViewportCandidates;

double ElapsedMilliseconds(const std::chrono::steady_clock::time_point& start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// The visual index's ranking is only allowed to narrow a cold start, never to decide for it. These
// bound how far it may narrow, and every one of them is deliberately on the side of searching more:
// the cost of a ranking that guessed wrong is the sweep that follows anyway, while the cost of a
// ranking trusted too far is a localisation the sweep would have found and this path did not.
//
// The winner has to lead by this factor before a cold start searches only it. Below the lead, the
// runner-up is kept as a second scene and the existing ambiguity check gets to see both - which is
// the honest answer when two coordinate planes look alike.
constexpr double kRetrievalSceneLead = 1.5;
// How much map data one cold-start window may carry, and how many tiles it may span.
//
// The window is chosen by RANK, not by radius. The minimap verifies its best-ranked tiles one at a
// time and that is what makes it fast; this path instead grew a radius until it covered every tile
// the ranking liked, which produced 92-98 MB candidates - twice what the matcher cache can hold, so
// such an entry could never survive to the next search and every cold start rebuilt it from scratch
// (the build log says "Entries1" for each one, against "Entries2" for the warm local-512 windows).
//
// The byte target is what a warm local-512 window costs - measured at 15-28 MB - which the cache
// demonstrably holds two of inside its 48 MB budget. A cold start keeps up to two scenes, so this
// is per scene. The tile cap only binds where tiles are sparse: it bounds how far apart candidates
// may sit when 20 MB is hundreds of them.
constexpr std::size_t kRetrievalHintBytes = 20 * 1024 * 1024;
constexpr std::size_t kRetrievalHintTiles = 64;
// The ladder's second rung: the same scenes, a slice of the same ranking, four times larger.
//
// It exists because the narrow slice sometimes misses - the ranking is right and the window simply
// does not reach the answer - and the only other rung was the sweep: nine whole scenes, 565 MB of
// matchers and 72 comparisons, measured at 11-23 seconds. One wider slice of a ranking that is
// already 17-for-18 is the same shape as the local-512 -> local-1024 ladder this path used to
// climb, and it costs one build instead of nine.
constexpr std::size_t kRetrievalWideBytes = 4 * kRetrievalHintBytes;
constexpr std::size_t kRetrievalWideTiles = 4 * kRetrievalHintTiles;


// The decision half of a match - fit, inliers, reprojection error, support - lives in a shared header so
// an alternative matcher can be judged by exactly the same gate as the shipped one. Bringing the names in
// keeps the call sites below unchanged.
using MapViewportMatch::TryMatch;


class MapViewportRuntime {
public:
    bool Initialize(std::shared_ptr<const RuntimeFeatureResources> resources, std::string& error) {
        std::scoped_lock lock(mutex_);
        if (ready_) return true;
        if (!resources || resources->map.imgDescriptors.empty() ||
            !resources->visualIndexReady || resources->visualIndex.tiles.empty()) {
            error = "map feature resources or scene index are unavailable";
            return false;
        }
        resources_ = std::move(resources);
        // Scene indices can be large. Construct them lazily on the
        // viewport worker when the user actually opens the map rather than
        // competing with capture/OCR/resource startup immediately after the
        // Start button is pressed.
        try {
            worker_ = std::jthread([this](std::stop_token token) { Worker(token); });
            ThreadPriority::MakeBackground(worker_);
            ready_ = true;
            error.clear();
            return true;
        } catch (const std::exception& exception) {
            ready_ = false;
            resources_.reset();
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
            if (worker_.joinable()) worker_.request_stop();
            condition_.notify_all();
            worker = std::move(worker_);
        }
        if (worker.joinable()) worker.join();
        std::scoped_lock lock(mutex_);
        ready_ = false;
        request_.reset();
        result_.reset();
        localMatchers_.clear();
        localMatcherBytes_ = 0;
        localMatcherEvictions_ = 0;
        localMatcherUseCounter_ = 0;
        vocabularyIndex_.release();
        resources_.reset();
    }

    bool Ready() const {
        std::scoped_lock lock(mutex_);
        return ready_;
    }

    bool Submit(MapViewportLocalizationRequest request) {
        std::scoped_lock lock(mutex_);
        if (!ready_) return false;
        // Superseded work stops at the next checkpoint. An OpenCV index build
        // itself is indivisible; its eventual result is still guarded by epoch.
        ++epoch_;
        result_.reset();
        request_ = std::move(request);
        condition_.notify_one();
        return true;
    }

    void CancelPending() {
        std::scoped_lock lock(mutex_);
        ++epoch_; request_.reset(); result_.reset();
    }

    bool Take(MapViewportLocalizationResult& result) {
        std::scoped_lock lock(mutex_);
        if (!result_) return false;
        result = std::move(*result_);
        result_.reset();
        return true;
    }

private:
    // One thing a request tries: what it may compare against, and who chose it.
    struct SearchPlan {
        std::vector<int> scenes;
        // The tiles to compare against, per scene, best-ranked first. A scene with no entry here is
        // searched whole; a scene with an EMPTY entry is searched not at all, which is how a prior
        // that covers no tiles keeps meaning "nothing to look at" rather than "look everywhere".
        std::map<int, std::vector<std::uint32_t>> candidates;
    };

    // What the visual index said about a cold start's viewport before anything was compared.
    struct SceneRanking {
        bool valid = false;
        int sceneId = 0;
        int runnerUpSceneId = 0;
        std::vector<int> scenes;
        // Each kept scene's scored tiles in rank order, best first. The plans slice this at
        // different budgets, which is what makes the ladder a matter of arithmetic rather than a
        // second ranking pass.
        std::map<int, std::vector<std::uint32_t>> rankedTiles;
        int rankedSceneCount = 0;
        double topScore = 0.0;
        double runnerUpScore = 0.0;
        int topCandidateCount = 0;
        double milliseconds = 0.0;
    };

    // Built on the viewport worker the first time a cold start asks for a ranking, for the same
    // reason the scene indices themselves are lazy: it must not compete with capture, OCR and
    // resource startup right after the Start button is pressed.
    cv::flann::Index* VocabularyIndex() {
        if (!vocabularyIndex_ && resources_) {
            vocabularyIndex_ = VisualIndexRetrieval::BuildVocabularyIndex(resources_->visualIndex.vocabulary);
        }
        return vocabularyIndex_.get();
    }

    // What one tile costs once it becomes part of a matcher: one 128-wide float descriptor and one
    // keypoint per row. This is the same quantity the matcher cache budgets, so choosing candidates
    // by it is choosing by the thing that decides whether they survive to the next search.
    std::size_t TileBytes(std::uint32_t tileIndex) const {
        if (tileIndex >= resources_->visualIndex.tiles.size()) return 0;
        return resources_->visualIndex.tiles[tileIndex].featureRowCount *
            static_cast<std::size_t>(MapVisualIndex::DescriptorColumns * sizeof(float) + sizeof(cv::KeyPoint));
    }

    // The tiles one scene's own ranking liked, best first. No radius: the ranking already says
    // which tiles the query describes, and covering the gaps between them is what made the
    // candidate set too big to keep.
    std::vector<std::uint32_t> RankSceneTiles(const std::vector<double>& scores, int sceneId) const {
        std::vector<std::pair<double, std::uint32_t>> liked;
        for (std::uint32_t tileIndex = 0; tileIndex < scores.size(); ++tileIndex) {
            if (!(scores[tileIndex] > 0.0)) continue;
            if (TileScene(*resources_, tileIndex) != sceneId) continue;
            liked.emplace_back(scores[tileIndex], tileIndex);
        }
        std::sort(liked.begin(), liked.end(), [](const auto& left, const auto& right) {
            if (left.first != right.first) return left.first > right.first;
            return left.second < right.second;
        });
        std::vector<std::uint32_t> ordered;
        ordered.reserve(liked.size());
        for (const auto& [score, tileIndex] : liked) ordered.push_back(tileIndex);
        return ordered;
    }

    // The best of a ranked list, taken in order until it reaches the byte target or the tile cap.
    std::vector<std::uint32_t> SliceCandidates(const std::vector<std::uint32_t>& rankedTiles,
        std::size_t byteBudget, std::size_t tileCap) const {
        std::vector<std::uint32_t> chosen;
        std::size_t bytes = 0;
        for (const auto tileIndex : rankedTiles) {
            if (chosen.size() >= tileCap) break;
            chosen.push_back(tileIndex);
            bytes += TileBytes(tileIndex);
            if (bytes >= byteBudget) break;
        }
        return chosen;
    }

    // Rank the index's tiles against one viewport crop.
    //
    // A cold start has no prior, so this is the only thing that can say which scene the player is
    // looking at without comparing against every scene in the game. It is the same ranking the
    // minimap runs (VisualIndexRetrieval); what differs is what this path does with it.
    SceneRanking RankScenes(const ImageFeatureData& features) {
        SceneRanking ranking;
        auto* vocabulary = VocabularyIndex();
        if (!vocabulary || !resources_) return ranking;
        const auto start = std::chrono::steady_clock::now();
        std::vector<double> scores;
        const bool scored = VisualIndexRetrieval::ScoreTiles(
            *resources_, *vocabulary, features.imgDescriptors, scores);
        ranking.milliseconds = ElapsedMilliseconds(start);
        if (!scored) return ranking;

        // One score per scene: the best tile it owns. Copies of the same region are alternate
        // observations rather than independent evidence, so the maximum is the same policy the
        // minimap applies inside a single grid cell. Taking the maximum rather than a sum is also
        // what keeps a large region from outranking a small one purely by having more tiles.
        std::map<int, double> best;
        for (std::uint32_t tileIndex = 0; tileIndex < scores.size(); ++tileIndex) {
            if (!(scores[tileIndex] > 0.0)) continue;
            const int sceneId = TileScene(*resources_, tileIndex);
            if (!Scene::IsRuntimeApproved(sceneId)) continue;
            auto& entry = best[sceneId];
            entry = std::max(entry, scores[tileIndex]);
        }
        if (best.empty()) return ranking;
        std::vector<std::pair<double, int>> ranked;
        ranked.reserve(best.size());
        for (const auto& [sceneId, score] : best) ranked.emplace_back(score, sceneId);
        std::sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
            if (left.first != right.first) return left.first > right.first;
            return left.second < right.second;
        });
        ranking.valid = true;
        ranking.rankedSceneCount = static_cast<int>(ranked.size());
        ranking.sceneId = ranked.front().second;
        ranking.topScore = ranked.front().first;
        ranking.runnerUpSceneId = ranked.size() > 1 ? ranked[1].second : 0;
        ranking.runnerUpScore = ranked.size() > 1 ? ranked[1].first : 0.0;

        // Keep the runner-up only when it cannot be separated from the winner. Two coordinate
        // planes this close are exactly the ambiguity the caller already knows how to reject, so
        // let that check see both - and either way each kept scene gets its own candidates below.
        ranking.scenes.push_back(ranking.sceneId);
        if (ranked.size() > 1 && ranking.runnerUpScore * kRetrievalSceneLead > ranking.topScore) {
            ranking.scenes.push_back(ranking.runnerUpSceneId);
        }

        // Every kept scene is ranked, or none is. Half a plan cannot be searched at one scale: the
        // caller narrows the zoom factors when a plan carries candidates, and a scene left without
        // them would then only ever be searched at the wrong one.
        std::map<int, std::vector<std::uint32_t>> rankedTiles;
        for (const int sceneId : ranking.scenes) {
            auto ordered = RankSceneTiles(scores, sceneId);
            if (ordered.empty()) return ranking;
            rankedTiles.emplace(sceneId, std::move(ordered));
        }
        ranking.topCandidateCount = static_cast<int>(SliceCandidates(
            rankedTiles.at(ranking.sceneId), kRetrievalHintBytes, kRetrievalHintTiles).size());
        ranking.rankedTiles = std::move(rankedTiles);
        return ranking;
    }

    // The plans a request tries, in order.
    //
    // Every request except a cold start gets exactly one plan: itself. A cold start gets up to
    // three, because the ranking is a hint and not an answer:
    //
    //   0. the ranking's own best tiles,
    //   1. the same ranking, sliced four times wider,
    //   2. the sweep this path has always run.
    //
    // Rung 1 is the one added because rung 0 misses: a ranking that is right but whose window does
    // not reach the answer used to fall straight through to nine whole scenes, measured at 11-23
    // seconds. The sweep stays as the floor - without it a ranking that named the wrong scene would
    // lose a localisation the sweep would have found - so the ranking can only beat it.
    std::vector<SearchPlan> BuildPlans(const MapViewportLocalizationRequest& request,
        const std::vector<int>& allScenes, const SceneRanking& ranking) {
        std::vector<SearchPlan> plans;
        if (ranking.valid && !ranking.scenes.empty()) {
            SearchPlan narrow;
            SearchPlan wide;
            narrow.scenes = wide.scenes = ranking.scenes;
            bool reachesFurther = false;
            for (const int sceneId : ranking.scenes) {
                const auto& ordered = ranking.rankedTiles.at(sceneId);
                // Not "near"/"far": those are legacy Windows macros and the compiler sees them
                // before it sees a variable name.
                auto closer = SliceCandidates(ordered, kRetrievalHintBytes, kRetrievalHintTiles);
                auto wider = SliceCandidates(ordered, kRetrievalWideBytes, kRetrievalWideTiles);
                reachesFurther = reachesFurther || wider.size() > closer.size();
                narrow.candidates.emplace(sceneId, std::move(closer));
                wide.candidates.emplace(sceneId, std::move(wider));
            }
            plans.push_back(std::move(narrow));
            // A rung that reaches no further than the one below it is not a rung.
            if (reachesFurther) plans.push_back(std::move(wide));
        }
        SearchPlan requested;
        requested.scenes = allScenes;
        if (request.prior.has_value() && request.prior->valid) {
            // An empty selection stays empty on purpose: a prior that covers no tiles has always
            // meant "there is nothing here", and turning that into "search the whole scene" would
            // be a different search wearing the same name.
            requested.candidates.emplace(request.prior->sceneId,
                SelectSceneTileIndices(*resources_, request.prior->sceneId, *request.prior));
        }
        plans.push_back(std::move(requested));
        return plans;
    }

    MapViewportLocalizationResult Locate(const MapViewportLocalizationRequest& request, const std::function<bool()>& interrupted) {
        MapViewportLocalizationResult result;
        result.sessionId = request.sessionId;
        result.uiGeneration = request.uiGeneration;
        result.viewportGeneration = request.viewportGeneration;
        result.frameId = request.frameId;
        result.requestId = request.requestId;
        result.viewportRevision = request.viewportRevision;
        result.scope = request.scope;
        const auto start = std::chrono::steady_clock::now();
        if (!resources_ || request.mapCrop.empty()) {
            result.durationMilliseconds = ElapsedMilliseconds(start);
            return result;
        }

        try {
            if (interrupted()) throw SearchInterrupted{};
            auto surf = cv::xfeatures2d::SURF::create(100, 4, 3, true, true);
            if (interrupted()) throw SearchInterrupted{};
            // Independent coordinate systems must never share one geometry fit.
            // Test each installed, approved scene and reject ambiguous scenes.
            std::vector<int> scenes;
            for (std::size_t i = 0; i < resources_->visualIndex.tiles.size(); ++i) {
                const int id = TileScene(*resources_, i);
                if (!Scene::IsRuntimeApproved(id)) continue;
                if (request.scope != MapViewportSearchScope::Global &&
                    (!request.prior || !request.prior->valid || request.prior->sceneId != id)) continue;
                if (std::find(scenes.begin(), scenes.end(), id) == scenes.end()) scenes.push_back(id);
            }
            const auto fullSize = request.mapCrop.size();
            const int centralWidth = std::max(1, cvRound(fullSize.width * 350.0 / 1280.0));
            const int centralHeight = std::max(1, cvRound(fullSize.height * 350.0 / 630.0));
            const std::array<cv::Rect, 2> regions = {
                cv::Rect((fullSize.width - centralWidth) / 2, (fullSize.height - centralHeight) / 2,
                    centralWidth, centralHeight), cv::Rect(0, 0, fullSize.width, fullSize.height)
            };
            // The map-centre crop at its native scale is both the ranking's query and the first
            // region the search compares, so it is extracted once and shared. Nothing else is
            // extracted early: a local scope, which is the common case, must not pay for a ranking
            // it will never use.
            SceneRanking ranking;
            std::optional<ImageFeatureData> centreFeatures;
            if (request.scope == MapViewportSearchScope::Global && scenes.size() > 1 && !regions[0].empty()) {
                if (interrupted()) throw SearchInterrupted{};
                ImageFeatureData features = FeatureMatch::ExtractSurfFeatures(surf, request.mapCrop(regions[0]));
                for (auto& point : features.imgKeypoints) point.pt += cv::Point2f(regions[0].x, regions[0].y);
                ranking = RankScenes(features);
                centreFeatures = std::move(features);
                result.retrievalRankedSceneCount = ranking.rankedSceneCount;
                result.retrievalSceneId = ranking.sceneId;
                result.retrievalRunnerUpSceneId = ranking.runnerUpSceneId;
                result.retrievalTopScore = ranking.topScore;
                result.retrievalRunnerUpScore = ranking.runnerUpScore;
                result.retrievalSceneCount = ranking.valid ? static_cast<int>(ranking.scenes.size()) : 0;
                result.retrievalTileCount = ranking.valid ? ranking.topCandidateCount : 0;
                result.retrievalMilliseconds = ranking.milliseconds;
            }
            const auto plans = BuildPlans(request, scenes, ranking);
            bool sceneAmbiguity = false;
            int acceptedPlan = -1;
            int planIndex = -1;
            // Plan 0's own tiles, kept so the answer can be measured against them once it is known.
            std::vector<std::uint32_t> narrowCandidates;
            for (const auto& plan : plans) {
            ++planIndex;
            if (result.accepted || sceneAmbiguity || plan.scenes.empty()) continue;
            ++result.plansRun;
            // A plan that carries candidates compares one scale, because they were ranked from
            // descriptors taken at one scale; a plan without them has to look for the scale too.
            const bool ranked = !plan.candidates.empty();
            for (std::size_t regionIndex = 0; regionIndex < regions.size(); ++regionIndex) {
            if (interrupted()) throw SearchInterrupted{};
            const auto& region = regions[regionIndex];
            const cv::Mat regionCrop = request.mapCrop(region);
            ImageFeatureData cropFeatures;
            if (regionIndex == 0 && centreFeatures.has_value()) {
                cropFeatures = *centreFeatures;
            }
            else {
                cropFeatures = FeatureMatch::ExtractSurfFeatures(surf, regionCrop);
                for (auto& point : cropFeatures.imgKeypoints) point.pt += cv::Point2f(region.x, region.y);
            }
            result.cropKeypointCount = static_cast<int>(cropFeatures.imgKeypoints.size());
            if (cropFeatures.imgDescriptors.empty()) continue;
            for (const double factor : { 1.0, 1.5, 2.0, 0.75 }) {
                if (factor != 1.0 && ranked) break;
                if (interrupted()) throw SearchInterrupted{};
                ImageFeatureData features = cropFeatures;
                if (factor != 1.0) {
                    cv::Mat resized;
                    cv::resize(regionCrop, resized, {}, factor, factor,
                        factor > 1.0 ? cv::INTER_CUBIC : cv::INTER_AREA);
                    features = FeatureMatch::ExtractSurfFeatures(surf, resized);
                    const double sx = static_cast<double>(resized.cols) / regionCrop.cols;
                    const double sy = static_cast<double>(resized.rows) / regionCrop.rows;
                    for (auto& point : features.imgKeypoints) {
                        point.pt.x = static_cast<float>((point.pt.x + 0.5) / sx - 0.5 + region.x);
                        point.pt.y = static_cast<float>((point.pt.y + 0.5) / sy - 0.5 + region.y);
                    }
                }
                if (features.imgDescriptors.empty()) continue;
                int acceptedScenes = 0;
                for (const int sceneId : plan.scenes) {
                    if (interrupted()) throw SearchInterrupted{};
                    // A scene the plan named is compared against exactly the tiles the plan chose.
                    // No entry means the whole of it, which is what the sweep does.
                    const auto chosen = plan.candidates.find(sceneId);
                    std::vector<std::uint32_t> whole;
                    const std::vector<std::uint32_t>* tiles = nullptr;
                    if (chosen == plan.candidates.end()) {
                        whole = SelectSceneTileIndices(*resources_, sceneId, std::nullopt);
                        tiles = &whole;
                    }
                    else {
                        tiles = &chosen->second;
                    }
                    auto* cache = GetLocalMatcher(*tiles);
                    if (!cache || !cache->matcher) continue;
                    if (planIndex == 0) narrowCandidates = *tiles;
                    std::vector<std::vector<cv::DMatch>> pairs;
                    cache->matcher->knnMatch(features.imgDescriptors, pairs, 2);
                    auto attempt = result;
                    attempt.accepted = false;
                    attempt.sceneId = sceneId;
                    attempt.goodMatchCount = attempt.inlierCount = attempt.coveredQuadrants = 0;
                    attempt.inlierRatio = attempt.medianReprojectionError = 0.0;
                    attempt.cropKeypointCount = static_cast<int>(features.imgKeypoints.size());
                    attempt.captureCorners.clear();
                    const auto shippedStart = std::chrono::steady_clock::now();
                    attempt.accepted = TryMatch(features, request.mapCrop, cache->candidates,
                        FilterGoodMatches(pairs), attempt);
                    const double shippedMs = ElapsedMilliseconds(shippedStart);
                    // The comparison runs on the same query and the same tiles, whether or not the shipped
                    // matcher accepted: a rejection the copy-free matcher would have accepted is exactly the
                    // kind of difference this is looking for.
                    if (attempt.accepted) ++acceptedScenes;
                    if (attempt.accepted || (!result.accepted &&
                        (attempt.inlierCount > result.inlierCount ||
                        (attempt.inlierCount == result.inlierCount && attempt.goodMatchCount > result.goodMatchCount))))
                        result = std::move(attempt);
                }
                if (acceptedScenes > 1) {
                    sceneAmbiguity = true;
                    result.accepted = false;
                    result.sceneId = 0;
                    result.captureCorners.clear();
                    break;
                }
                if (result.accepted) break;
            }
            if (result.accepted || sceneAmbiguity) break;
            }
            if (result.accepted) acceptedPlan = planIndex;
            }
            result.acceptedPlan = acceptedPlan;
            // How far plan 0's own candidates sat from the position this search accepted. It is the
            // measurement that separates "the candidates never reached the answer" from "they
            // reached it and the match failed anyway", and it can only be taken once the answer is
            // known - which is why it lives here rather than in the ranking.
            if (result.accepted && !narrowCandidates.empty()) {
                double nearest = std::numeric_limits<double>::infinity();
                double reach = 0.0;
                for (const auto tileIndex : narrowCandidates) {
                    if (tileIndex >= resources_->visualIndex.tiles.size()) continue;
                    const auto& tile = resources_->visualIndex.tiles[tileIndex];
                    const double dx = std::max(0.0, std::max(static_cast<double>(tile.minX) - result.centerMapCoordinate.x,
                        result.centerMapCoordinate.x - static_cast<double>(tile.maxX)));
                    const double dy = std::max(0.0, std::max(static_cast<double>(tile.minY) - result.centerMapCoordinate.y,
                        result.centerMapCoordinate.y - static_cast<double>(tile.maxY)));
                    const double distance = std::hypot(dx, dy);
                    nearest = std::min(nearest, distance);
                    reach = std::max(reach, distance);
                }
                if (std::isfinite(nearest)) {
                    result.candidateNearestM = nearest;
                    result.candidateReachM = reach;
                }
            }
        }
        catch (const cv::Exception&) {
            result.accepted = false;
        }
        catch (const std::exception&) {
            result.accepted = false;
        }
        result.durationMilliseconds = ElapsedMilliseconds(start);
        return result;
    }

    struct LocalMatcherCache {
        ImageFeatureData candidates;
        cv::Ptr<cv::FlannBasedMatcher> matcher;
        std::uint64_t lastUsed = 0;
        std::size_t bytes = 0;
    };

    LocalMatcherCache* GetLocalMatcher(const std::vector<std::uint32_t>& tileIndices) {
        if (tileIndices.empty()) return nullptr;
        const std::string key = TileSetKey(tileIndices);
        const bool trace = Diagnostics::MemoryTraceEnabled();
        const auto found = localMatchers_.find(key);
        if (found != localMatchers_.end()) {
            found->second.lastUsed = ++localMatcherUseCounter_;
            // A hit means this map window was paid for on an earlier visit: the difference between a player
            // who stays in one place and one who teleports is exactly how often this branch is taken.
            if (trace) Diagnostics::Record("local-matcher-cache", "result=hit tiles=" + std::to_string(tileIndices.size()) +
                " keypoints=" + std::to_string(found->second.candidates.imgKeypoints.size()) +
                " entries=" + std::to_string(localMatchers_.size()));
            return &found->second;
        }
        // The cache is bounded by bytes, not by entry count. Counting entries read as "24 small things",
        // while each entry is a candidate set of 6k-36k keypoints plus its FLANN index, 5-19 MB and 12.7 MB
        // on average, so the old cap of 24 could hold 370 MB.
        //
        // The budget is deliberately small. Two measurements settled it (2026-10-02): with a 320 MB budget
        // the cache filled to 24 entries and a roaming session peaked at 1783 MB, while keeping a single
        // entry - every window rebuilt and discarded - held the working set flat around 700 MB through 151
        // rebuilds and 1.8 GB of allocation. So the resident pages came from how many sets were held at
        // once, not from rebuilding: the allocator reuses the freed slots when the same sizes come back.
        // A few entries keep the easy hits (the window the player is in and its neighbours) without turning
        // the cache into a second copy of the map.
        //
        // IMAO_LOCAL_MATCHER_CACHE=1 keeps a single entry, which is the experiment that established this.
        static const bool singleEntry = [] {
            char value[8]{}; std::size_t length = 0;
            return getenv_s(&length, value, sizeof(value), "IMAO_LOCAL_MATCHER_CACHE") == 0 && value[0] == '1';
        }();
        // The budget is the dial between two costs that pull against each other, measured on this machine
        // (2026-10-02): every search that misses rebuilds a candidate copy and trains a FLANN index over it,
        // while the shipped knnMatch itself costs 0.1-0.4 ms. A larger budget means fewer rebuilds and more
        // memory held; a smaller one means the reverse. IMAO_LOCAL_MATCHER_BYTES_MB overrides it, so the
        // settings can be compared one session each without a rebuild - the choice is seconds of latency
        // against hundreds of megabytes, and that is not a choice to make by estimate.
        const std::size_t configuredBudgetMb = [] {
            char value[16]{}; std::size_t length = 0;
            if (getenv_s(&length, value, sizeof(value), "IMAO_LOCAL_MATCHER_BYTES_MB") != 0 || value[0] == '\0') return std::size_t{0};
            return static_cast<std::size_t>(std::strtoul(value, nullptr, 10));
        }();
        const std::size_t configuredEntries = [] {
            char value[16]{}; std::size_t length = 0;
            if (getenv_s(&length, value, sizeof(value), "IMAO_LOCAL_MATCHER_ENTRIES") != 0 || value[0] == '\0') return std::size_t{0};
            return static_cast<std::size_t>(std::strtoul(value, nullptr, 10));
        }();
        const std::size_t byteBudget = singleEntry ? 0
            : (configuredBudgetMb > 0 ? configuredBudgetMb * 1024 * 1024 : 48 * 1024 * 1024);
        const std::size_t entryBudget = singleEntry ? 1 : (configuredEntries > 0 ? configuredEntries : 8);
        LocalMatcherCache cache;
        cache.candidates = SelectSceneCandidates(*resources_, tileIndices);
        if (cache.candidates.imgDescriptors.empty()) return nullptr;
        cache.matcher = cv::makePtr<cv::FlannBasedMatcher>();
        cache.matcher->add(std::vector<cv::Mat>{ cache.candidates.imgDescriptors });
        cache.matcher->train();
        cache.lastUsed = ++localMatcherUseCounter_;
        cache.bytes = cache.candidates.imgDescriptors.total() * cache.candidates.imgDescriptors.elemSize() +
            cache.candidates.imgKeypoints.capacity() * sizeof(cv::KeyPoint);
        // Evict until this entry fits, oldest first. Evictions are counted because they are the difference
        // between a cache that grows to its budget once and one that churns: a rebuild costs the bytes it
        // allocates plus the FLANN index trained over them, and churn is what the trace showed at 1.3
        // searches a second.
        while (!localMatchers_.empty() &&
            (localMatcherBytes_ + cache.bytes > byteBudget ||
                localMatchers_.size() >= entryBudget)) {
            const auto oldest = std::min_element(localMatchers_.begin(), localMatchers_.end(),
                [](const auto& left, const auto& right) {
                    return left.second.lastUsed < right.second.lastUsed;
                });
            if (oldest == localMatchers_.end()) break;
            localMatcherBytes_ -= oldest->second.bytes;
            localMatchers_.erase(oldest);
            ++localMatcherEvictions_;
        }
        if (trace) Diagnostics::Record("local-matcher-cache", "result=miss tiles=" + std::to_string(tileIndices.size()) +
            " keypoints=" + std::to_string(cache.candidates.imgKeypoints.size()) + " bytes=" +
            std::to_string(cache.bytes) + " entries=" + std::to_string(localMatchers_.size()) +
            " held=" + std::to_string(localMatcherBytes_) + " budget=" + std::to_string(byteBudget) +
            " evictions=" + std::to_string(localMatcherEvictions_) +
            (singleEntry ? " mode=single-entry" : ""));
        const auto inserted = localMatchers_.emplace(key, std::move(cache));
        localMatcherBytes_ += inserted.first->second.bytes;
        // A memory reading on every rebuild, not on a timer: the question is whether the pages a rebuild
        // allocates go back to the system when the entry is evicted, and only a reading taken here can
        // answer it. Misses are rare enough (tens a session) that this stays cheap.
        Diagnostics::RecordMemory(std::string("local-matcher-built") + (singleEntry ? "-single" : "") +
            "Mb" + std::to_string(inserted.first->second.bytes / (1024 * 1024)) +
            "Entries" + std::to_string(localMatchers_.size()));
        return &inserted.first->second;
    }

    void Worker(std::stop_token token) {
        while (!token.stop_requested()) {
            MapViewportLocalizationRequest request;
            uint64_t epoch = 0;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [&] { return token.stop_requested() || request_.has_value(); });
                if (token.stop_requested()) return;
                epoch = epoch_.load();
                request = std::move(*request_);
                request_.reset();
            }
            auto result = Locate(request, [&] { return token.stop_requested() || epoch_.load() != epoch; });
            {
                std::scoped_lock lock(mutex_);
                if (!token.stop_requested() && epoch_.load() == epoch) result_ = std::move(result);
            }
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::jthread worker_;
    std::atomic_uint64_t epoch_{0};
    bool ready_ = false;
    std::shared_ptr<const RuntimeFeatureResources> resources_;
    std::unordered_map<std::string, LocalMatcherCache> localMatchers_;
    std::size_t localMatcherBytes_ = 0;
    std::uint64_t localMatcherEvictions_ = 0;
    std::uint64_t localMatcherUseCounter_ = 0;
    std::optional<MapViewportLocalizationRequest> request_;
    std::optional<MapViewportLocalizationResult> result_;
    // The vocabulary search the cold-start ranking needs. Built lazily on the worker thread and
    // never touched by any other, so it needs no lock of its own.
    cv::Ptr<cv::flann::Index> vocabularyIndex_;
};

MapViewportRuntime& Runtime() {
    static MapViewportRuntime runtime;
    return runtime;
}
}

bool MapViewportLocalizer::Initialize(std::shared_ptr<const RuntimeFeatureResources> resources, std::string& error) {
    return Runtime().Initialize(std::move(resources), error);
}

void MapViewportLocalizer::Shutdown() {
    Runtime().Shutdown();
}

bool MapViewportLocalizer::IsReady() {
    return Runtime().Ready();
}

bool MapViewportLocalizer::Submit(MapViewportLocalizationRequest request) {
    return Runtime().Submit(std::move(request));
}

bool MapViewportLocalizer::TryTakeLatestResult(MapViewportLocalizationResult& result) {
    return Runtime().Take(result);
}

const char* MapViewportLocalizer::ScopeName(MapViewportSearchScope scope) {
    switch (scope) {
    case MapViewportSearchScope::Local512: return "local-512";
    case MapViewportSearchScope::Global: return "global";
    }
    return "unknown";
}

void MapViewportLocalizer::CancelPending() { Runtime().CancelPending(); }
