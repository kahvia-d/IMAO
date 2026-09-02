#include "MapViewportLocalizer.h"

#include "../Coordinate/locationCalculator/MapCoordinate.h"

#include <opencv2/xfeatures2d.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
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

ImageFeatureData SelectWorldCandidates(const RuntimeFeatureResources& resources,
    const std::optional<WorldSearchPrior>& prior) {
    // Build the verification set from the visual-index rows rather than
    // merely filtering all map keypoints by X/Y.  This keeps an independent
    // state with overlapping internal coordinates out of a World search, and
    // includes every approved World supplement (such as BlackShores) exactly
    // once.
    if (!resources.visualIndexReady || resources.visualIndex.tiles.empty()) {
        return resources.map;
    }

    std::vector<unsigned char> selected(resources.map.imgKeypoints.size(), 0);
    for (std::size_t tileIndex = 0; tileIndex < resources.visualIndex.tiles.size(); ++tileIndex) {
        const auto& tile = resources.visualIndex.tiles[tileIndex];
        if (!IsWorldTile(resources, tileIndex) ||
            (prior.has_value() && prior->valid && !IntersectsPrior(tile, *prior))) {
            continue;
        }
        const std::size_t first = tile.featureRowOffset;
        const std::size_t last = first + tile.featureRowCount;
        if (last > resources.visualIndex.featureRows.size()) continue;
        for (std::size_t index = first; index < last; ++index) {
            const auto row = resources.visualIndex.featureRows[index];
            if (row < selected.size()) selected[row] = 1;
        }
    }

    ImageFeatureData candidates;
    for (std::size_t row = 0; row < selected.size(); ++row) {
        if (!selected[row]) continue;
        candidates.imgKeypoints.push_back(resources.map.imgKeypoints[row]);
        candidates.imgDescriptors.push_back(resources.map.imgDescriptors.row(static_cast<int>(row)));
    }
    return candidates;
}

bool TryMatch(const ImageFeatureData& cropFeatures, const cv::Mat& crop,
    const ImageFeatureData& candidateFeatures, MapViewportLocalizationResult& result) {
    if (cropFeatures.imgDescriptors.empty() || candidateFeatures.imgDescriptors.empty()) return false;
    const auto goodMatches = FeatureMatch::FindGoodMatchesFLANN(cropFeatures, candidateFeatures, 0.62f, 0.50f);
    result.goodMatchCount = static_cast<int>(goodMatches.size());
    if (result.goodMatchCount < 8) return false;

    Coordinate center;
    std::vector<cv::Point2f> corners;
    cv::Mat cropForProjection = crop;
    if (!MapCoordinate::GetMapCoordinateOfCenterGameMapPos(candidateFeatures, cropFeatures,
        goodMatches, cropForProjection, center, corners) || corners.size() != 4) {
        return false;
    }
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
        ready_ = true;
        worker_ = std::jthread([this](std::stop_token token) { Worker(token); });
        return true;
    }

    void Shutdown() {
        std::jthread worker;
        {
            std::scoped_lock lock(mutex_);
            if (worker_.joinable()) worker_.request_stop();
            condition_.notify_all();
            worker = std::move(worker_);
        }
        if (worker.joinable()) worker.join();
        std::scoped_lock lock(mutex_);
        ready_ = false;
        request_.reset();
        result_.reset();
        resources_.reset();
    }

    bool Ready() const {
        std::scoped_lock lock(mutex_);
        return ready_;
    }

    bool Submit(MapViewportLocalizationRequest request) {
        std::scoped_lock lock(mutex_);
        if (!ready_) return false;
        // The newest screenshot is the only useful queued request.  A running
        // request is allowed to finish, but its generation/frame is checked by
        // App before it can affect the viewport.
        request_ = std::move(request);
        condition_.notify_one();
        return true;
    }

    bool Take(MapViewportLocalizationResult& result) {
        std::scoped_lock lock(mutex_);
        if (!result_) return false;
        result = std::move(*result_);
        result_.reset();
        return true;
    }

private:
    MapViewportLocalizationResult Locate(const MapViewportLocalizationRequest& request) const {
        MapViewportLocalizationResult result;
        result.sessionId = request.sessionId;
        result.uiGeneration = request.uiGeneration;
        result.viewportGeneration = request.viewportGeneration;
        result.frameId = request.frameId;
        result.scope = request.scope;
        const auto start = std::chrono::steady_clock::now();
        if (!resources_ || request.mapCrop.empty()) {
            result.durationMilliseconds = ElapsedMilliseconds(start);
            return result;
        }

        try {
            auto surf = cv::xfeatures2d::SURF::create(100, 4, 3, true, true);
            const ImageFeatureData cropFeatures = FeatureMatch::ExtractSurfFeatures(surf, request.mapCrop);
            result.cropKeypointCount = static_cast<int>(cropFeatures.imgKeypoints.size());
            if (cropFeatures.imgDescriptors.empty()) {
                result.durationMilliseconds = ElapsedMilliseconds(start);
                return result;
            }

            const auto localPrior = request.scope == MapViewportSearchScope::Global
                ? std::optional<WorldSearchPrior>{} : request.prior;
            ImageFeatureData candidates = SelectWorldCandidates(*resources_, localPrior);
            result.accepted = TryMatch(cropFeatures, request.mapCrop, candidates, result);
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

    void Worker(std::stop_token token) {
        while (!token.stop_requested()) {
            MapViewportLocalizationRequest request;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [&] { return token.stop_requested() || request_.has_value(); });
                if (token.stop_requested()) return;
                request = std::move(*request_);
                request_.reset();
            }
            auto result = Locate(request);
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
