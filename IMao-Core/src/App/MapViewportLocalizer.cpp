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
// Both bounds are four times the narrow rung's, in tiles and in keypoints. Preparation costs about
// 16 microseconds per distinct keypoint, measured on the real index (5,037 keypoints -> 80 ms,
// 23,611 -> 375 ms, 596,694 -> 9,282 ms), so the wide rung costs roughly 375 ms to build where the
// narrow one costs 82 ms - and that is the whole difference between them.
//
// The sweep's own budget is smaller, and for a different reason: it keeps one window per scene, and
// all nine have to survive in the matcher cache at once. A window that does not fit evicts one that
// does, and the sweep then rebuilds all nine on each of its eight crops and zooms. The cache
// budgets 48 MB, so a window may spend a ninth of it and leave room over.
//
// Measured from the field log: with the narrow rung's budget the sweep's nine windows came to about
// 90 MB, and the one sweep that had to run took 13,058 ms - almost exactly the 72 rebuilds at
// 185 ms each that thrashing implies. At a ninth of the cache the nine come to 36 MB, they all
// stay, and the sweep is nine preparations instead of seventy-two.
constexpr std::size_t kSweepBytes = 4 * 1024 * 1024;
constexpr std::size_t kSweepTiles = 64;
// The zooms a full-screen search tries, in the order it tries them. The ranking uses the same list:
// it ranks the map-centre crop at each of them and carries the one it was ranked at into the plan,
// because a crop rendered at one zoom is a different picture to the index than the same crop at
// another, and ranking it at the wrong one picks the wrong scene.
constexpr std::array<double, 4> kFactors = { 1.0, 1.5, 2.0, 0.75 };
// How many of those rankings become plans. Every one of them is a scene that led at some zoom, and
// the field evidence is a ranking that named the game's largest scene whenever the zoom was not the
// one the index was built at - so the plan list carries the leaders rather than betting on one.
constexpr std::size_t kRankedPlans = 3;


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
        // Whether this plan compares one scale, and which. A plan whose candidates were ranked from
        // descriptors taken at one scale compares that scale; the sweep keeps looking for the scale
        // too, because "the crop is zoomed" is a failure the ranking cannot see.
        bool singleScale = true;
        double factor = 1.0;
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
        // Every scene that scored at all, in rank order. The sweep compares against the best of each
        // of them, which is what lets it stay cheap without giving up on a scene the ranking rejected.
        std::vector<int> scoredScenes;
        int rankedSceneCount = 0;
        double topScore = 0.0;
        double runnerUpScore = 0.0;
        int topCandidateCount = 0;
        double milliseconds = 0.0;
        // Which zoom this ranking was taken at, and how decisively it separated its winner. A crop
        // rendered at the zoom the index was built at ranks its own scene far ahead of the rest; at
        // any other zoom the scores bunch up and the largest scene wins on the number of tiles it
        // owns. The ratio is that separation, and it orders the plans.
        double factor = 1.0;
        double ratio = 0.0;
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
    //
    // The budget is spent on DISTINCT keypoints, not on the tiles' row counts. A tile's
    // featureRowCount counts references, and tiles overlap - 384 units wide on a 192-unit stride,
    // with six packs stacked inside the World scene - so the same map point is referenced by many
    // tiles at once. Measured on World: 49 tiles carried 38,800 references but only 5,037 distinct
    // keypoints, a factor of 7.7. Spending the budget on references bought an eighth of the
    // candidates it was meant to, which is why a "20 MB" window was really 2.6 MB and why the
    // field log's retrievalTiles sat at 5-17.
    std::vector<std::uint32_t> SliceCandidates(const std::vector<std::uint32_t>& rankedTiles,
        std::size_t byteBudget, std::size_t tileCap) const {
        constexpr std::size_t kBytesPerKeypoint =
            MapVisualIndex::DescriptorColumns * sizeof(float) + sizeof(cv::KeyPoint);
        std::vector<std::uint32_t> chosen;
        std::vector<unsigned char> counted(resources_->map.imgKeypoints.size(), 0);
        std::size_t keypoints = 0;
        for (const auto tileIndex : rankedTiles) {
            if (chosen.size() >= tileCap) break;
            if (tileIndex >= resources_->visualIndex.tiles.size()) continue;
            const auto& tile = resources_->visualIndex.tiles[tileIndex];
            for (std::uint32_t row = 0; row < tile.featureRowCount; ++row) {
                const auto index = tile.featureRowOffset + row;
                if (index >= resources_->visualIndex.featureRows.size()) break;
                const auto mapRow = resources_->visualIndex.featureRows[index];
                if (mapRow >= counted.size() || counted[mapRow] != 0) continue;
                counted[mapRow] = 1;
                ++keypoints;
            }
            chosen.push_back(tileIndex);
            if (keypoints * kBytesPerKeypoint >= byteBudget) break;
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
        ranking.ratio = ranking.runnerUpScore > 0.0 ? ranking.topScore / ranking.runnerUpScore : 0.0;

        // Keep the runner-up only when it cannot be separated from the winner. Two coordinate
        // planes this close are exactly the ambiguity the caller already knows how to reject, so
        // let that check see both - and either way each kept scene gets its own candidates below.
        ranking.scenes.push_back(ranking.sceneId);
        if (ranked.size() > 1 && ranking.runnerUpScore * kRetrievalSceneLead > ranking.topScore) {
            ranking.scenes.push_back(ranking.runnerUpSceneId);
        }

        // Every scored scene is ranked, not only the kept ones: the sweep compares against the best
        // of every scene, and it needs their tiles for that.
        std::map<int, std::vector<std::uint32_t>> rankedTiles;
        std::vector<int> scoredScenes;
        for (const auto& [score, sceneId] : ranked) {
            auto ordered = RankSceneTiles(scores, sceneId);
            if (ordered.empty()) continue;
            rankedTiles.emplace(sceneId, std::move(ordered));
            scoredScenes.push_back(sceneId);
        }
        // All or nothing for the kept scenes: a plan that carries candidates compares one scale, and
        // a kept scene without them would then only ever be searched at the wrong one.
        for (const int sceneId : ranking.scenes) {
            if (rankedTiles.find(sceneId) == rankedTiles.end()) return ranking;
        }
        ranking.scoredScenes = std::move(scoredScenes);
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
    //   0. the ranking's own best tiles, for the scenes it kept,
    //   1. the same ranking sliced four times wider,
    //   2. every scene that scored, each against its own best tiles, at every zoom.
    //
    // Rung 2 used to compare against whole scenes - 3,967 tiles and 596,694 keypoints for World,
    // measured at 9.2 seconds to prepare one matcher. Nine scenes against a cache that holds eight
    // meant it prepared them all again on every one of its eight crops and zooms, so a single
    // search could still be running after a minute. Ranking each scene's own tiles costs 0.08 s to
    // prepare instead of 9.2, and nine of those fit the budget at once, so the churn disappears with
    // the cost. It keeps all four zooms, because a crop that is scaled is a failure the ranking
    // cannot see.
    std::vector<SearchPlan> BuildPlans(const MapViewportLocalizationRequest& request,
        const std::vector<int>& allScenes, const std::vector<SceneRanking>& rankings) {
        std::vector<SearchPlan> plans;
        // Most decisive zoom first. Each ranking carries the zoom it was taken at, and its plan
        // compares at that zoom - a window is a set of tiles the query matched at one scale, and
        // comparing it at another is asking a question it was not built to answer.
        std::vector<const SceneRanking*> ordered;
        for (const auto& ranking : rankings) {
            if (ranking.valid && !ranking.scenes.empty()) ordered.push_back(&ranking);
        }
        std::sort(ordered.begin(), ordered.end(), [](const SceneRanking* left, const SceneRanking* right) {
            if (left->ratio != right->ratio) return left->ratio > right->ratio;
            return left->factor < right->factor;
        });
        if (ordered.size() > kRankedPlans) ordered.resize(kRankedPlans);

        for (const auto* ranking : ordered) {
            SearchPlan narrow;
            narrow.factor = ranking->factor;
            narrow.scenes = ranking->scenes;
            for (const int sceneId : ranking->scenes) {
                narrow.candidates.emplace(sceneId, SliceCandidates(
                    ranking->rankedTiles.at(sceneId), kRetrievalHintBytes, kRetrievalHintTiles));
            }
            plans.push_back(std::move(narrow));
        }
        // One wider rung, at the most decisive zoom: the ranking can be right about the scene and
        // still not have reached the answer, and widening that one costs a build where the sweep
        // costs nine.
        if (!ordered.empty()) {
            const auto* best = ordered.front();
            SearchPlan wide;
            wide.factor = best->factor;
            wide.scenes = best->scenes;
            bool reachesFurther = false;
            for (const int sceneId : best->scenes) {
                const auto& rankedTiles = best->rankedTiles.at(sceneId);
                auto wider = SliceCandidates(rankedTiles, kRetrievalWideBytes, kRetrievalWideTiles);
                reachesFurther = reachesFurther || wider.size() > plans.front().candidates.at(sceneId).size();
                wide.candidates.emplace(sceneId, std::move(wider));
            }
            // A rung that reaches no further than the one below it is not a rung.
            if (reachesFurther) plans.push_back(std::move(wide));
        }
        // The sweep compares every scene that scored, against its own best tiles, at every zoom.
        // Nine windows of a ninth of the cache each: 26 MB resident and 729 ms to prepare all of
        // them, measured, against the 9.2 seconds one whole World matcher takes. It used to compare
        // whole scenes, which is why a search could still be running after a minute.
        if (!ordered.empty()) {
            const auto* best = ordered.front();
            SearchPlan sweep;
            sweep.singleScale = false;
            for (const int sceneId : best->scoredScenes) {
                const auto ranked = best->rankedTiles.find(sceneId);
                if (ranked == best->rankedTiles.end()) continue;
                sweep.candidates.emplace(sceneId, SliceCandidates(ranked->second, kSweepBytes, kSweepTiles));
            }
            sweep.scenes = best->scoredScenes;
            if (!sweep.scenes.empty()) plans.push_back(std::move(sweep));
        }
        // A global request stops here. The windowed sweep above is already its last rung, and
        // appending the exhaustive whole-scene search after it puts back exactly the cost this
        // change removed: the field log shows one request running the windowed sweep and then
        // 563 MB of whole-scene matchers, twice through World alone, until the watchdog cancelled it
        // at twenty seconds. A global request only reaches for whole scenes when the ranking cannot
        // run at all - an index with no usable vocabulary, which can never rank anything - and then
        // the exhaustive search is the only way to answer rather than a slower way to answer.
        if (request.scope == MapViewportSearchScope::Global) {
            if (!plans.empty()) return plans;
            if (VocabularyIndex() != nullptr) {
                // The ranking could have run and did not, which means the map-centre crop was not
                // usable: the player was panning, or the map was mid-transition. Reporting no answer
                // costs a quarter of a second and the next frame ranks normally; grinding through
                // whole scenes costs twenty.
                Diagnostics::Record("map-viewport-noranking",
                    "frame=" + std::to_string(request.frameId) + " scenes=" + std::to_string(allScenes.size()));
                return plans;
            }
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
            // The map-centre crop at native scale is both the ranking's query and the first region
            // the search compares, so it is extracted once and shared. Nothing else is extracted
            // early: a local scope, which is the common case, must not pay for a ranking it will
            // never use.
            //
            // The ranking runs at every zoom the search will use, not just the native one. A crop
            // rendered at one zoom is a different picture to the index than the same crop at
            // another, and the field log shows what ranking it at the wrong one costs: a World
            // screenshot ranked Lahai first at factor 2.0 and 0.25 while ranking World first at
            // 1.0, 1.5, 0.75 and 0.5. Lahai is the largest scene in the game, so at any zoom the
            // index was not built at, the scene with the most tiles wins.
            std::vector<SceneRanking> rankings;
            std::array<std::optional<ImageFeatureData>, kFactors.size()> centreByFactor;
            if (request.scope == MapViewportSearchScope::Global && scenes.size() > 1 && !regions[0].empty()) {
                for (std::size_t factorIndex = 0; factorIndex < kFactors.size(); ++factorIndex) {
                    if (interrupted()) throw SearchInterrupted{};
                    const double factor = kFactors[factorIndex];
                    cv::Mat query = request.mapCrop(regions[0]);
                    if (factor != 1.0) {
                        cv::resize(request.mapCrop(regions[0]), query, {}, factor, factor,
                            factor > 1.0 ? cv::INTER_CUBIC : cv::INTER_AREA);
                        const double sx = static_cast<double>(query.cols) / regions[0].width;
                        const double sy = static_cast<double>(query.rows) / regions[0].height;
                        ImageFeatureData features = FeatureMatch::ExtractSurfFeatures(surf, query);
                        for (auto& point : features.imgKeypoints) {
                            point.pt.x = static_cast<float>((point.pt.x + 0.5) / sx - 0.5 + regions[0].x);
                            point.pt.y = static_cast<float>((point.pt.y + 0.5) / sy - 0.5 + regions[0].y);
                        }
                        centreByFactor[factorIndex] = std::move(features);
                    }
                    else {
                        ImageFeatureData features = FeatureMatch::ExtractSurfFeatures(surf, query);
                        for (auto& point : features.imgKeypoints) point.pt += cv::Point2f(regions[0].x, regions[0].y);
                        centreByFactor[factorIndex] = std::move(features);
                    }
                    auto ranking = RankScenes(*centreByFactor[factorIndex]);
                    ranking.factor = factor;
                    rankings.push_back(std::move(ranking));
                }
                // The result reports the most decisive ranking, which is the one whose plan runs
                // first; the rest are in plansRun and acceptedPlan.
                for (const auto& ranking : rankings) {
                    if (!ranking.valid) continue;
                    if (result.retrievalRankedSceneCount == 0 || ranking.ratio > result.retrievalTopScore) {
                        result.retrievalRankedSceneCount = ranking.rankedSceneCount;
                        result.retrievalSceneId = ranking.sceneId;
                        result.retrievalRunnerUpSceneId = ranking.runnerUpSceneId;
                        result.retrievalTopScore = ranking.ratio;
                        result.retrievalRunnerUpScore = ranking.runnerUpScore;
                        result.retrievalSceneCount = static_cast<int>(ranking.scenes.size());
                        result.retrievalTileCount = ranking.topCandidateCount;
                    }
                    result.retrievalMilliseconds += ranking.milliseconds;
                }
            }
            const auto plans = BuildPlans(request, scenes, rankings);
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
            // descriptors taken at one scale; the sweep keeps looking for the scale as well.
            const bool ranked = plan.singleScale;
            for (std::size_t regionIndex = 0; regionIndex < regions.size(); ++regionIndex) {
            if (interrupted()) throw SearchInterrupted{};
            const auto& region = regions[regionIndex];
            const cv::Mat regionCrop = request.mapCrop(region);
            // Region 0's crops at every zoom were already extracted for the ranking, so they are not
            // extracted again here. Region 1, and every case the ranking did not run for, is.
            const bool centreIsExtracted = regionIndex == 0 && centreByFactor[0].has_value();
            std::optional<ImageFeatureData> wholeRegion;
            if (!centreIsExtracted) {
                ImageFeatureData features = FeatureMatch::ExtractSurfFeatures(surf, regionCrop);
                for (auto& point : features.imgKeypoints) point.pt += cv::Point2f(region.x, region.y);
                wholeRegion = std::move(features);
            }
            result.cropKeypointCount = static_cast<int>(centreIsExtracted
                ? centreByFactor[0]->imgKeypoints.size() : wholeRegion->imgKeypoints.size());
            if (!centreIsExtracted && wholeRegion->imgDescriptors.empty()) continue;
            for (std::size_t factorIndex = 0; factorIndex < kFactors.size(); ++factorIndex) {
                const double factor = kFactors[factorIndex];
                // A ranked plan compares the one zoom its candidates were ranked at; the sweep is
                // the only plan that still has to look for the zoom.
                if (ranked && factor != plan.factor) continue;
                if (interrupted()) throw SearchInterrupted{};
                ImageFeatureData features;
                // The map-centre crop at each zoom was already extracted for the ranking. Reusing it
                // costs nothing, and re-extracting it nine times over the sweep's scenes would cost
                // more than the sweep itself.
                if (regionIndex == 0 && centreByFactor[factorIndex].has_value()) {
                    features = *centreByFactor[factorIndex];
                }
                else if (factor == 1.0 && wholeRegion.has_value()) {
                    features = *wholeRegion;
                }
                else {
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
                    const bool keep = attempt.accepted || (!result.accepted &&
                        (attempt.inlierCount > result.inlierCount ||
                        (attempt.inlierCount == result.inlierCount && attempt.goodMatchCount > result.goodMatchCount)));
                    if (keep) {
                        // The candidates travel with the attempt they produced. Taking them from
                        // the loop instead recorded whichever scene happened to be visited last -
                        // in the two-scene case that is the runner-up, which is how this field
                        // first reported the answer sitting 5 km from its own candidate set.
                        if (planIndex == 0) narrowCandidates = *tiles;
                        result = std::move(attempt);
                    }
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
        // Sixteen entries, not eight. The count bound exists so the cache cannot grow into a second
        // copy of the map, and the byte budget is what actually enforces that; but eight was below
        // the nine scenes a sweep compares, so the sweep's ninth matcher evicted its first and every
        // pass rebuilt all nine. At the sizes a window now has - about 3 MB each - nine of them fit
        // inside the byte budget with room to spare, and the count bound was the only thing left
        // making the sweep churn.
        const std::size_t entryBudget = singleEntry ? 1 : (configuredEntries > 0 ? configuredEntries : 16);
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
