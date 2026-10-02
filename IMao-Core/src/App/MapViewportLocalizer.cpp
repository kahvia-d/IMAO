#include <atomic>
#include <functional>
#include "../Feature/Match/ExactDescriptorMatcher.h"
#include "MapViewportLocalizer.h"
#include "MapViewportGeometry.h"
#include "MapViewportCandidates.h"
#include "MapViewportMatch.h"
#include "../Feature/Match/UniqueMapFeatures.h"
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


// The decision half of a match - fit, inliers, reprojection error, support - lives in a shared header so
// an alternative matcher can be judged by exactly the same gate as the shipped one. Bringing the names in
// keeps the call sites below unchanged.
using MapViewportMatch::TryMatch;
using MapViewportMatch::TryMatchRows;

// The comparison that decides whether the viewport search can stop building a de-duplicated copy of the map
// rows for every window. With IMAO_MATCHER_AB=1 the same query, the same rows and the same gate are run a
// second time through the copy-free matcher, and the two outcomes are recorded side by side. It is the only
// way to compare them on a real query: an offline query cannot be brought onto the features' own scale, so
// the offline attempts matched nothing and could say nothing (2026-10-02). Measurement only, off by default
// - with it on each search does its work twice and the map answers more slowly.
const bool matcherAbEnabled = [] {
    char value[8]{}; std::size_t length = 0;
    return getenv_s(&length, value, sizeof(value), "IMAO_MATCHER_AB") == 0 && value[0] == '1';
}();

// Nearest and second-nearest descriptor distances for a sample of the query, measured the same way the
// matcher measures them. The shipped path finds hundreds of good pairs while the copy-free path finds none,
// and the difference can only be the ratio test or the distance ceiling - the two numbers below say which,
// instead of leaving it to be guessed (2026-10-02).
std::string NearestDistanceSummary(const RuntimeFeatureResources& resources,
    const std::vector<std::uint32_t>& rows, const ImageFeatureData& query) {
    if (rows.empty() || query.imgDescriptors.empty()) return "dist=none";
    std::vector<std::vector<cv::DMatch>> forward, reverse;
    MatchDescriptorsExactly(resources.map.imgDescriptors, query.imgDescriptors, forward, reverse);
    // Both directions over the same data, in one run, because getting this wrong is the failure that cost a
    // day: the ratio test has to be taken per query descriptor, over its two nearest map rows. Taken per map
    // row instead - over the row's two nearest query descriptors - it measures a different quantity, and row
    // and query counts differ by three orders of magnitude here. The two counts below should differ wildly;
    // if they do not, the direction is not what is wrong.
    std::vector<float> best(query.imgDescriptors.rows, std::numeric_limits<float>::max());
    std::vector<float> second(query.imgDescriptors.rows, std::numeric_limits<float>::max());
    std::vector<int> bestRow(query.imgDescriptors.rows, -1);
    MatchDescriptorNeighbours(query.imgDescriptors, resources.map.imgDescriptors, rows, best, second, bestRow);
    int queryRatio = 0;
    for (int index = 0; index < query.imgDescriptors.rows; ++index) {
        if (bestRow[index] < 0) continue;
        if (second[index] > 0.0f && best[index] >= 0.62f * second[index]) continue;
        if (best[index] > 0.50f) continue;
        ++queryRatio;
    }
    std::vector<double> nearest, ratios;
    int passRatio = 0;
    const std::size_t step = std::max<std::size_t>(1, rows.size() / 64);
    for (std::size_t position = 0; position < rows.size(); position += step) {
        const auto row = rows[position];
        if (row >= forward.size() || forward[row].size() < 2) continue;
        nearest.push_back(forward[row][0].distance);
        if (forward[row][1].distance > 0.0f) ratios.push_back(forward[row][0].distance / forward[row][1].distance);
        if (forward[row][0].distance >= 0.62f * forward[row][1].distance ||
            forward[row][0].distance > 0.50f) continue;
        ++passRatio;
    }
    if (nearest.empty()) return "dist=none";
    const auto middle = [](std::vector<double> values) {
        std::sort(values.begin(), values.end());
        return values.empty() ? 0.0 : values[values.size() / 2];
    };
    int underCeiling = 0;
    for (const auto distance : nearest) if (distance <= 0.50) ++underCeiling;
    return "nearestP50=" + std::to_string(middle(nearest)) +
        " ratioP50=" + std::to_string(middle(ratios)) +
        " underCeiling=" + std::to_string(underCeiling) + "/" + std::to_string(nearest.size()) +
        " rowRatio=" + std::to_string(passRatio) + " queryRatio=" + std::to_string(queryRatio);
}

// One row per compared search: what the shipped matcher decided, what the copy-free matcher decided on the
// same query, and how far apart the two positions are. centerDistance is the number that matters - a
// matcher that finds the same place faster is a candidate; one that finds a different place is not.
void RecordMatcherComparison(const MapViewportLocalizationResult& shipped,
    const MapViewportLocalizationResult& exact, double exactMs, std::size_t rows, double shippedMs) {
    // Capped per session: a search runs about once a second while the map is open, and a flood of lines is
    // both unreadable and slow enough to distort what is being measured.
    static std::atomic_int comparisons = 0;
    if (comparisons.fetch_add(1) >= 200) return;
    double distance = -1.0;
    if (shipped.accepted && exact.accepted) distance = std::hypot(shipped.centerMapCoordinate.x - exact.centerMapCoordinate.x,
        shipped.centerMapCoordinate.y - exact.centerMapCoordinate.y);
    Diagnostics::Record("matcher-ab", "scene=" + std::to_string(shipped.sceneId) +
        " rows=" + std::to_string(rows) +
        " shippedAccepted=" + std::to_string(shipped.accepted ? 1 : 0) +
        " shippedGood=" + std::to_string(shipped.goodMatchCount) +
        " shippedInliers=" + std::to_string(shipped.inlierCount) +
        " shippedMs=" + std::to_string(shippedMs) +
        " exactAccepted=" + std::to_string(exact.accepted ? 1 : 0) +
        " exactGood=" + std::to_string(exact.goodMatchCount) +
        " exactInliers=" + std::to_string(exact.inlierCount) +
        " exactMs=" + std::to_string(exactMs) +
        " centerDistance=" + std::to_string(distance));
}


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
            bool sceneAmbiguity = false;
            for (const auto& region : regions) {
            if (interrupted()) throw SearchInterrupted{};
            const cv::Mat regionCrop = request.mapCrop(region);
            ImageFeatureData cropFeatures = FeatureMatch::ExtractSurfFeatures(surf, regionCrop);
            for (auto& point : cropFeatures.imgKeypoints) point.pt += cv::Point2f(region.x, region.y);
            result.cropKeypointCount = static_cast<int>(cropFeatures.imgKeypoints.size());
            if (cropFeatures.imgDescriptors.empty()) continue;
            for (const double factor : { 1.0, 1.5, 2.0, 0.75 }) {
                if (factor != 1.0 && request.scope != MapViewportSearchScope::Global) break;
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
                for (const int sceneId : scenes) {
                    if (interrupted()) throw SearchInterrupted{};
                    const auto tiles = SelectSceneTileIndices(*resources_, sceneId,
                        request.scope == MapViewportSearchScope::Global ? std::nullopt : request.prior);
                    auto* cache = GetLocalMatcher(tiles);
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
                    if (matcherAbEnabled) {
                        const auto abRows = SelectCandidateRows(*resources_, tiles);
                        MapViewportLocalizationResult exactAttempt;
                        exactAttempt.accepted = false;
                        exactAttempt.sceneId = sceneId;
                        exactAttempt.cropKeypointCount = static_cast<int>(features.imgKeypoints.size());
                        const auto exactStart = std::chrono::steady_clock::now();
                        const auto exactPairs = MatchRowsExactly(*resources_, abRows, features.imgDescriptors);
                        const auto exactGood = DeduplicatePairs(*resources_, abRows, exactPairs);
                        exactAttempt.accepted = TryMatchRows(features, request.mapCrop, *resources_, abRows,
                            exactGood, exactAttempt);
                        const double exactMs = ElapsedMilliseconds(exactStart);
                        // The raw counts are what diagnoses the copy-free path when it finds nothing while the
                        // shipped one finds hundreds: whether the pairs never survived the ratio test, or
                        // survived and were then dropped by the vote-per-point filter.
                        Diagnostics::Record("matcher-ab-pairs", "candidates=" + std::to_string(cache->candidates.imgKeypoints.size()) +
                            " rows=" + std::to_string(abRows.size()) +
                            " queryKeypoints=" + std::to_string(features.imgKeypoints.size()) +
                            " queryColumns=" + std::to_string(features.imgDescriptors.cols) +
                            " mapColumns=" + std::to_string(resources_->map.imgDescriptors.cols) +
                            " exactRaw=" + std::to_string(exactPairs.size()) +
                            " exactDedup=" + std::to_string(exactGood.size()) + " " + NearestDistanceSummary(*resources_, abRows, features));
                        RecordMatcherComparison(attempt, exactAttempt, exactMs, abRows.size(), shippedMs);
                    }
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
        const std::size_t byteBudget = singleEntry ? 0 : 48 * 1024 * 1024;
        const std::size_t entryBudget = singleEntry ? 1 : 8;
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
    case MapViewportSearchScope::Local1024: return "local-1024";
    case MapViewportSearchScope::Global: return "global";
    }
    return "unknown";
}

void MapViewportLocalizer::CancelPending() { Runtime().CancelPending(); }
