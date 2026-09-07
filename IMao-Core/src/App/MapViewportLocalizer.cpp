#include <atomic>
#include <functional>
#include "../Feature/Match/ExactDescriptorMatcher.h"
#include "MapViewportLocalizer.h"
#include "MapViewportGeometry.h"
#include "../Feature/Match/UniqueMapFeatures.h"

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
double ElapsedMilliseconds(const std::chrono::steady_clock::time_point& start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

bool IsWorldTile(const RuntimeFeatureResources& resources, std::size_t index) {
    // The original state-8 index predates per-tile scene metadata.  It is all
    // World by definition; later registered packs carry their explicit scene.
    return index < resources.baseVisualTileCount ||
        (index < resources.visualIndex.tiles.size() &&
            resources.visualIndex.tiles[index].sceneId == Scene::SceneNameToId("World"));
}

bool IntersectsPrior(const MapVisualTile& tile, const WorldSearchPrior& prior) {
    const double nearestX = std::clamp(prior.centerMapCoordinate.x,
        static_cast<double>(tile.minX), static_cast<double>(tile.maxX));
    const double nearestY = std::clamp(prior.centerMapCoordinate.y,
        static_cast<double>(tile.minY), static_cast<double>(tile.maxY));
    return std::hypot(prior.centerMapCoordinate.x - nearestX,
        prior.centerMapCoordinate.y - nearestY) <= prior.radius;
}

std::vector<std::uint32_t> SelectWorldTileIndices(const RuntimeFeatureResources& resources,
    const std::optional<WorldSearchPrior>& prior) {
    // Build the verification set from the visual-index rows rather than
    // merely filtering all map keypoints by X/Y.  This keeps an independent
    // state with overlapping internal coordinates out of a World search, and
    // includes every approved World supplement (such as BlackShores) exactly
    // once.
    if (!resources.visualIndexReady || resources.visualIndex.tiles.empty()) {
        return {};
    }

    std::vector<std::uint32_t> selected;
    for (std::size_t tileIndex = 0; tileIndex < resources.visualIndex.tiles.size(); ++tileIndex) {
        const auto& tile = resources.visualIndex.tiles[tileIndex];
        if (!IsWorldTile(resources, tileIndex) ||
            (prior.has_value() && prior->valid && !IntersectsPrior(tile, *prior))) {
            continue;
        }
        selected.push_back(static_cast<std::uint32_t>(tileIndex));
    }
    return selected;
}

ImageFeatureData SelectWorldCandidates(const RuntimeFeatureResources& resources,
    const std::vector<std::uint32_t>& tileIndices) {
    if (tileIndices.empty()) return resources.map;
    std::vector<unsigned char> selected(resources.map.imgKeypoints.size(), 0);
    for (const auto tileIndex : tileIndices) {
        if (tileIndex >= resources.visualIndex.tiles.size()) continue;
        const auto& tile = resources.visualIndex.tiles[tileIndex];
        const std::size_t first = tile.featureRowOffset;
        const std::size_t last = first + tile.featureRowCount;
        if (last > resources.visualIndex.featureRows.size()) continue;
        for (std::size_t index = first; index < last; ++index) {
            const auto row = resources.visualIndex.featureRows[index];
            if (row < selected.size()) selected[row] = 1;
        }
    }
    ImageFeatureData candidates;
    UniqueMapFeatures unique;
    for (std::size_t row = 0; row < selected.size(); ++row) {
        if (!selected[row]) continue;
        if (!unique.Insert(resources.map.imgKeypoints[row],
                resources.map.imgDescriptors.row(static_cast<int>(row)))) continue;
        candidates.imgKeypoints.push_back(resources.map.imgKeypoints[row]);
        candidates.imgDescriptors.push_back(resources.map.imgDescriptors.row(static_cast<int>(row)));
    }
    return candidates;
}

std::string TileSetKey(const std::vector<std::uint32_t>& tileIndices) {
    std::string key;
    key.reserve(tileIndices.size() * 6);
    for (const auto tileIndex : tileIndices) {
        key += std::to_string(tileIndex);
        key.push_back(',');
    }
    return key;
}

std::vector<cv::DMatch> FilterGoodMatches(const std::vector<std::vector<cv::DMatch>>& pairs) {
    std::vector<cv::DMatch> goodMatches;
    for (const auto& pair : pairs) {
        if (pair.size() < 2 || pair[0].distance >= 0.62f * pair[1].distance || pair[0].distance > 0.50f) continue;
        goodMatches.push_back(pair[0]);
    }
    return goodMatches;
}

bool TryMatch(const ImageFeatureData& cropFeatures, const cv::Mat& crop,
    const ImageFeatureData& candidateFeatures, const std::vector<cv::DMatch>& goodMatches,
    MapViewportLocalizationResult& result) {
    if (cropFeatures.imgDescriptors.empty() || candidateFeatures.imgDescriptors.empty()) return false;
    result.goodMatchCount = static_cast<int>(goodMatches.size());
    if (result.goodMatchCount < 12) return false;

    std::vector<cv::Point2f> cropPoints;
    std::vector<cv::Point2f> mapPoints;
    cropPoints.reserve(goodMatches.size());
    mapPoints.reserve(goodMatches.size());
    for (const auto& match : goodMatches) {
        if (match.queryIdx < 0 || match.trainIdx < 0 ||
            match.queryIdx >= static_cast<int>(cropFeatures.imgKeypoints.size()) ||
            match.trainIdx >= static_cast<int>(candidateFeatures.imgKeypoints.size())) continue;
        cropPoints.push_back(cropFeatures.imgKeypoints[match.queryIdx].pt);
        mapPoints.push_back(candidateFeatures.imgKeypoints[match.trainIdx].pt);
    }
    if (cropPoints.size() < 12) return false;

    cv::Mat inlierMask;
    const cv::Mat homography = FitMapViewportTransform(cropPoints, mapPoints, inlierMask);
    if (homography.empty() || inlierMask.empty()) return false;
    result.inlierCount = cv::countNonZero(inlierMask);
    result.inlierRatio = static_cast<double>(result.inlierCount) / cropPoints.size();
    if (result.inlierCount < 12 || result.inlierRatio < 0.35) return false;

    const cv::Point2f cropCenter(crop.cols / 2.0f, crop.rows / 2.0f);
    std::array<bool, 4> quadrants{};
    std::vector<cv::Point2f> projected;
    cv::perspectiveTransform(cropPoints, projected, homography);
    std::vector<double> errors;
    errors.reserve(static_cast<std::size_t>(result.inlierCount));
    for (int index = 0; index < inlierMask.rows; ++index) {
        if (inlierMask.at<std::uint8_t>(index) == 0) continue;
        const auto& source = cropPoints[static_cast<std::size_t>(index)];
        const auto& expected = mapPoints[static_cast<std::size_t>(index)];
        const auto& mapped = projected[static_cast<std::size_t>(index)];
        errors.push_back(cv::norm(mapped - expected));
        const int quadrant = (source.x >= cropCenter.x ? 1 : 0) + (source.y >= cropCenter.y ? 2 : 0);
        quadrants[quadrant] = true;
    }
    std::sort(errors.begin(), errors.end());
    result.medianReprojectionError = errors.empty() ? 0.0 : errors[errors.size() / 2];
    result.coveredQuadrants = static_cast<int>(std::count(quadrants.begin(), quadrants.end(), true));
    if (!HasMapViewportSupport(cropPoints, inlierMask, crop.size()) ||
        result.medianReprojectionError > 3.0) return false;

    std::vector<cv::Point2f> cropCorners = {
        { 0.0f, 0.0f }, { static_cast<float>(crop.cols), 0.0f },
        { static_cast<float>(crop.cols), static_cast<float>(crop.rows) },
        { 0.0f, static_cast<float>(crop.rows) }
    };
    std::vector<cv::Point2f> corners;
    cv::perspectiveTransform(cropCorners, corners, homography);
    if (corners.size() != 4) return false;
    Coordinate center((corners[0].x + corners[2].x) / 2.0,
        (corners[0].y + corners[2].y) / 2.0);
    const double width = cv::norm(corners[1] - corners[0]);
    const double height = cv::norm(corners[3] - corners[0]);
    if (!std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(width) ||
        !std::isfinite(height) || width < 50.0 || height < 50.0) {
        return false;
    }
    result.centerMapCoordinate = center;
    result.captureCorners = std::move(corners);
    return true;
}

class MapViewportRuntime {
public:
    bool Initialize(std::shared_ptr<const RuntimeFeatureResources> resources, std::string& error) {
        std::scoped_lock lock(mutex_);
        if (ready_) return true;
        if (!resources || resources->map.imgDescriptors.empty()) {
            error = "map feature resources are unavailable";
            return false;
        }
        resources_ = std::move(resources);
        // The full World index can be large.  Construct it once on the
        // viewport worker when the user actually opens the map rather than
        // competing with capture/OCR/resource startup immediately after the
        // Start button is pressed.
        globalWorldIndexAttempted_ = false;
        try {
            worker_ = std::jthread([this](std::stop_token token) { Worker(token); });
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
        globalWorldCandidates_ = {};
        globalWorldMatcher_.release();
        localMatchers_.clear();
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
            const ImageFeatureData cropFeatures = FeatureMatch::ExtractSurfFeatures(surf, request.mapCrop);
            result.cropKeypointCount = static_cast<int>(cropFeatures.imgKeypoints.size());
            if (cropFeatures.imgDescriptors.empty()) {
                result.durationMilliseconds = ElapsedMilliseconds(start);
                return result;
            }

            if (interrupted()) throw SearchInterrupted{};
            const ImageFeatureData* candidates = nullptr;
            std::vector<cv::DMatch> goodMatches;
            if (request.scope == MapViewportSearchScope::Global) {
                if (!EnsureGlobalWorldMatcher()) {
                    result.durationMilliseconds = ElapsedMilliseconds(start);
                    return result;
                }
                if (interrupted()) throw SearchInterrupted{};
                candidates = &globalWorldCandidates_;
                if (!globalWorldMatcher_) {
                    result.durationMilliseconds = ElapsedMilliseconds(start);
                    return result;
                }
                std::vector<std::vector<cv::DMatch>> pairs;
                globalWorldMatcher_->knnMatch(cropFeatures.imgDescriptors, pairs, 2);
                goodMatches = FilterGoodMatches(pairs);
            }
            else {
                const auto tileIndices = SelectWorldTileIndices(*resources_, request.prior);
                if (tileIndices.empty() && resources_->visualIndexReady) {
                    result.durationMilliseconds = ElapsedMilliseconds(start);
                    return result;
                }
                LocalMatcherCache* cache = GetLocalMatcher(tileIndices);
                if (cache == nullptr || !cache->matcher) {
                    result.durationMilliseconds = ElapsedMilliseconds(start);
                    return result;
                }
                if (interrupted()) throw SearchInterrupted{};
                candidates = &cache->candidates;
                std::vector<std::vector<cv::DMatch>> pairs;
                cache->matcher->knnMatch(cropFeatures.imgDescriptors, pairs, 2);
                goodMatches = FilterGoodMatches(pairs);
            }
            result.accepted = candidates != nullptr && TryMatch(cropFeatures, request.mapCrop,
                *candidates, goodMatches, result);
            // A zoomed-out map loses descriptor agreement at the native crop
            // scale. Retry only global recovery, keeping normal tracking cheap.
            // Convert feature positions back to the original crop before fitting
            // so marker projection and support checks retain their coordinates.
            if (!result.accepted && candidates != nullptr &&
                request.scope == MapViewportSearchScope::Global) {
                for (const double factor : { 1.5, 2.0, 0.75 }) {
                    if (interrupted()) throw SearchInterrupted{};
                    cv::Mat resized;
                    cv::resize(request.mapCrop, resized, {}, factor, factor,
                        factor > 1.0 ? cv::INTER_CUBIC : cv::INTER_AREA);
                    auto features = FeatureMatch::ExtractSurfFeatures(surf, resized);
                    if (features.imgDescriptors.empty()) continue;
                    const double sx = static_cast<double>(resized.cols) / request.mapCrop.cols;
                    const double sy = static_cast<double>(resized.rows) / request.mapCrop.rows;
                    for (auto& point : features.imgKeypoints) {
                        point.pt.x = static_cast<float>((point.pt.x + 0.5) / sx - 0.5);
                        point.pt.y = static_cast<float>((point.pt.y + 0.5) / sy - 0.5);
                    }
                    std::vector<std::vector<cv::DMatch>> pairs;
                    globalWorldMatcher_->knnMatch(features.imgDescriptors, pairs, 2);
                    auto attempt = result;
                    attempt.goodMatchCount = attempt.inlierCount = attempt.coveredQuadrants = 0;
                    attempt.inlierRatio = attempt.medianReprojectionError = 0.0;
                    attempt.cropKeypointCount = static_cast<int>(features.imgKeypoints.size());
                    attempt.accepted = TryMatch(features, request.mapCrop, *candidates,
                        FilterGoodMatches(pairs), attempt);
                    if (attempt.accepted || attempt.inlierCount > result.inlierCount ||
                        (attempt.inlierCount == result.inlierCount && attempt.goodMatchCount > result.goodMatchCount))
                        result = std::move(attempt);
                    if (result.accepted) break;
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
    };

    bool EnsureGlobalWorldMatcher() {
        if (globalWorldMatcher_) return true;
        if (globalWorldIndexAttempted_ || !resources_) return false;
        globalWorldIndexAttempted_ = true;
        try {
            globalWorldCandidates_ = SelectWorldCandidates(*resources_,
                SelectWorldTileIndices(*resources_, std::nullopt));
            if (globalWorldCandidates_.imgDescriptors.empty()) {
                globalWorldCandidates_ = {};
                return false;
            }
            globalWorldMatcher_ = cv::makePtr<cv::FlannBasedMatcher>();
            globalWorldMatcher_->add(std::vector<cv::Mat>{ globalWorldCandidates_.imgDescriptors });
            globalWorldMatcher_->train();
            return true;
        }
        catch (const cv::Exception&) {
            globalWorldCandidates_ = {};
            globalWorldMatcher_.release();
            return false;
        }
        catch (const std::exception&) {
            globalWorldCandidates_ = {};
            globalWorldMatcher_.release();
            return false;
        }
    }

    LocalMatcherCache* GetLocalMatcher(const std::vector<std::uint32_t>& tileIndices) {
        const std::string key = TileSetKey(tileIndices);
        const auto found = localMatchers_.find(key);
        if (found != localMatchers_.end()) {
            found->second.lastUsed = ++localMatcherUseCounter_;
            return &found->second;
        }
        constexpr std::size_t kMaximumLocalMatcherCaches = 24;
        if (localMatchers_.size() >= kMaximumLocalMatcherCaches) {
            const auto oldest = std::min_element(localMatchers_.begin(), localMatchers_.end(),
                [](const auto& left, const auto& right) {
                    return left.second.lastUsed < right.second.lastUsed;
                });
            if (oldest != localMatchers_.end()) localMatchers_.erase(oldest);
        }
        LocalMatcherCache cache;
        cache.candidates = SelectWorldCandidates(*resources_, tileIndices);
        if (cache.candidates.imgDescriptors.empty()) return nullptr;
        cache.matcher = cv::makePtr<cv::FlannBasedMatcher>();
        cache.matcher->add(std::vector<cv::Mat>{ cache.candidates.imgDescriptors });
        cache.matcher->train();
        cache.lastUsed = ++localMatcherUseCounter_;
        const auto inserted = localMatchers_.emplace(key, std::move(cache));
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
    ImageFeatureData globalWorldCandidates_;
    cv::Ptr<cv::FlannBasedMatcher> globalWorldMatcher_;
    bool globalWorldIndexAttempted_ = false;
    std::unordered_map<std::string, LocalMatcherCache> localMatchers_;
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
