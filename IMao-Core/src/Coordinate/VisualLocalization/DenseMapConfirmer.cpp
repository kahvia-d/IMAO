#include "DenseMapConfirmer.h"

#include "../../Runtime/ResourceSnapshotContext.h"
#include "../CoordinateStruct.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace DenseMapConfirmer {
namespace {
// The tile raster: 1024 pixels per 850 world units, as the packs' manifests record.
constexpr double kTilePixelsPerUnit = 1024.0 / 850.0;
// The minimap is normalised to a square before features are extracted; the mask radii
// below are expressed in that same space (42 = the player arrow, 80 = the terrain ring).
constexpr double kNormalizedMinimapSize = 184.0;
constexpr double kPlayerMarkerRadius = 42.0;
constexpr double kTerrainRadius = 80.0;
// +-64 map pixels is about 53 world units: the priors we confirm are exact to a few
// units, so anything wider only buys ambiguity.
constexpr int kSearchRadius = 64;
// The window has to be almost entirely covered by real tiles; a hole would confirm noise.
constexpr double kMinimumCoverage = 0.9;

std::mutex cacheMutex;
std::unordered_map<std::string, cv::Mat> tileCache;
constexpr std::size_t kCacheLimit = 12;

std::string TileKey(int state, int x, int y) {
    return std::to_string(state) + ":" + std::to_string(x) + ":" + std::to_string(y);
}

// Decoded tiles are 1 MB each and a confirmation touches at most four of them, so a
// small cache turns the per-frame cost into the correlation alone.
cv::Mat LoadTile(const std::filesystem::path& root, int state, int x, int y) {
    const auto key = TileKey(state, x, y);
    {
        std::scoped_lock lock(cacheMutex);
        const auto found = tileCache.find(key);
        if (found != tileCache.end()) return found->second;
    }
    const auto path = root / std::to_string(state) /
        (std::to_string(state) + "_" + std::to_string(x) + "_" + std::to_string(y) + ".png");
    cv::Mat loaded = cv::imread(path.string(), cv::IMREAD_GRAYSCALE);
    if (!loaded.empty() && loaded.type() != CV_8U) loaded.convertTo(loaded, CV_8U);
    {
        std::scoped_lock lock(cacheMutex);
        if (tileCache.size() >= kCacheLimit) tileCache.clear();
        tileCache[key] = loaded;
    }
    return loaded;
}

std::string Numbers(double score, double peak, double offsetX, double offsetY) {
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(4);
    stream << "score=" << score << " peak=" << peak;
    stream.precision(1);
    stream << " peakOffset=(" << offsetX << "," << offsetY << ")";
    return stream.str();
}
}

std::filesystem::path ReferenceRoot() {
    // A development or self-built tree may stage the tile archive beside the program;
    // shipped installs do not carry map imagery yet, and then this stays empty.
    const auto besideProgram = ResourceSnapshotContext::ExecutableDirectory() / "MapTiles";
    std::error_code error;
    if (std::filesystem::is_directory(besideProgram, error)) return besideProgram;
    if (ResourceSnapshotContext::Configured()) {
        const auto besideBaseline = ResourceSnapshotContext::BaselineRoot() / "MapTiles";
        if (std::filesystem::is_directory(besideBaseline, error)) return besideBaseline;
    }
    return {};
}

Result Confirm(const cv::Mat& normalizedMinimap, int sceneId, const cv::Point2d& prior, double terrainScale) {
    return Confirm(normalizedMinimap, sceneId, prior, terrainScale, ReferenceRoot());
}

Result Confirm(const cv::Mat& normalizedMinimap, int sceneId, const cv::Point2d& prior, double terrainScale,
    const std::filesystem::path& referenceRoot) {
    const auto start = std::chrono::steady_clock::now();
    Result result;
    const auto finish = [&](const std::string& detail) {
        result.milliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        std::ostringstream stream;
        stream.setf(std::ios::fixed);
        stream.precision(1);
        stream << detail << " durationMs=" << result.milliseconds;
        result.detail = stream.str();
        return result;
    };
    if (normalizedMinimap.empty() || normalizedMinimap.channels() != 1 || !(terrainScale > 0.0) ||
        !std::isfinite(prior.x) || !std::isfinite(prior.y)) return finish("reason=input-unusable");
    const auto* scene = Scene::Find(sceneId);
    if (scene == nullptr || scene->kuroStateId <= 0) return finish("reason=unknown-scene");
    if (referenceRoot.empty()) return finish("reason=no-reference");

    // Map pixels per minimap pixel: the tiles are 1.2047 px per world unit and the
    // normalised minimap covers kNormalizedMinimapSize * terrainScale units.
    const double scale = kTilePixelsPerUnit * terrainScale;
    const int templateWidth = static_cast<int>(std::lround(normalizedMinimap.cols * scale));
    const int templateHeight = static_cast<int>(std::lround(normalizedMinimap.rows * scale));
    if (templateWidth < 32 || templateHeight < 32) return finish("reason=template-too-small");

    // The reference window, centred on the prior, in map pixels.
    const int side = std::max(templateWidth, templateHeight) + 2 * kSearchRadius;
    const int originX = static_cast<int>(std::lround(prior.x - side / 2.0));
    const int originY = static_cast<int>(std::lround(prior.y - side / 2.0));
    cv::Mat window = cv::Mat::zeros(side, side, CV_8U);
    long long covered = 0;
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            const int tileX = static_cast<int>(std::floor(originX / 1024.0)) + i;
            const int tileY = static_cast<int>(std::floor(originY / 1024.0)) + j;
            const cv::Mat tile = LoadTile(referenceRoot, scene->kuroStateId, tileX, tileY);
            const int destinationX = tileX * 1024 - originX, destinationY = tileY * 1024 - originY;
            const int sourceX = std::max(0, -destinationX), sourceY = std::max(0, -destinationY);
            const int targetX = std::max(0, destinationX), targetY = std::max(0, destinationY);
            const int width = std::min(1024 - sourceX, side - targetX);
            const int height = std::min(1024 - sourceY, side - targetY);
            if (width <= 0 || height <= 0) continue;
            if (tile.empty() || tile.cols < 1024 || tile.rows < 1024) continue;
            tile(cv::Rect(sourceX, sourceY, width, height))
                .copyTo(window(cv::Rect(targetX, targetY, width, height)));
            covered += static_cast<long long>(width) * height;
        }
    }
    if (covered < kMinimumCoverage * side * side) return finish("reason=reference-incomplete");

    // The template: the minimap at map scale, masked to the ring the runtime trusts.
    cv::Mat templateImage;
    cv::resize(normalizedMinimap, templateImage, cv::Size(templateWidth, templateHeight), 0.0, 0.0, cv::INTER_LINEAR);
    templateImage.convertTo(templateImage, CV_32F);
    cv::Mat mask = cv::Mat::zeros(templateHeight, templateWidth, CV_32F);
    const double innerRadius = kPlayerMarkerRadius / kNormalizedMinimapSize * templateWidth;
    const double outerRadius = kTerrainRadius / kNormalizedMinimapSize * templateWidth;
    cv::circle(mask, cv::Point(templateWidth / 2, templateHeight / 2), static_cast<int>(std::lround(outerRadius)),
        cv::Scalar(1), cv::FILLED);
    cv::circle(mask, cv::Point(templateWidth / 2, templateHeight / 2), static_cast<int>(std::lround(innerRadius)),
        cv::Scalar(0), cv::FILLED);
    const double maskSum = cv::sum(mask)[0];
    if (maskSum < 0.2 * mask.total()) return finish("reason=mask-empty");
    const double templateMean = cv::sum(templateImage.mul(mask))[0] / maskSum;
    cv::Mat centered = (templateImage - templateMean).mul(mask);

    // Zero-mean normalised correlation, computed by hand so the ring mask applies:
    // numerator and the local image energy both slide with the window.
    cv::Mat windowFloat, windowSquared;
    window.convertTo(windowFloat, CV_32F);
    cv::multiply(windowFloat, windowFloat, windowSquared);
    cv::Mat numerator, localSum, localEnergy;
    cv::matchTemplate(windowFloat, centered, numerator, cv::TM_CCORR);
    cv::matchTemplate(windowFloat, mask, localSum, cv::TM_CCORR);
    cv::matchTemplate(windowSquared, mask, localEnergy, cv::TM_CCORR);
    const double templateEnergy = cv::sum(centered.mul(centered))[0];
    cv::Mat variance = localEnergy - localSum.mul(localSum) / maskSum;
    cv::max(variance, 0.0, variance);
    cv::Mat denominator;
    cv::sqrt(variance * templateEnergy, denominator);
    cv::Mat score;
    cv::divide(numerator, denominator, score);
    cv::patchNaNs(score, 0.0);

    double peak = 0.0;
    cv::Point peakAt;
    cv::minMaxLoc(score, nullptr, &peak, nullptr, &peakAt);
    // The template's top-left corner sits kSearchRadius from the window's corner when the
    // prior is the centre, so that cell is the correlation *at the prior*.
    const int priorX = std::max(0, std::min(score.cols - 1, (side - templateWidth) / 2));
    const int priorY = std::max(0, std::min(score.rows - 1, (side - templateHeight) / 2));
    result.available = true;
    result.score = score.at<float>(priorY, priorX);
    result.peakScore = peak;
    result.peakOffsetX = peakAt.x - priorX;
    result.peakOffsetY = peakAt.y - priorY;
    const double offset = std::hypot(result.peakOffsetX, result.peakOffsetY);
    result.accepted = result.score >= MinimumScore && offset <= MaximumPeakOffsetPixels;
    return finish(Numbers(result.score, result.peakScore, result.peakOffsetX, result.peakOffsetY));
}
}
