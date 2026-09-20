#include "DenseMapConfirmer.h"

#include "../../Runtime/ResourceSnapshotContext.h"
#include "../CoordinateStruct.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace DenseMapConfirmer {
namespace {
// The tile archive is the game's own grid: one 1024-pixel tile per 850 world units, as
// the packs' manifests record.
constexpr int kTileSize = 1024;
constexpr double kTilePixelsPerUnit = kTileSize / 850.0;
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

// Where a map pixel lives on the game's tile grid.  The archive is cut the way the game
// names its tiles, and that is not a plain mosaic of the map raster:
//   * the column block grows with map x, and tile 0 starts one whole tile to the left of
//     the map origin (the game's tileX = floor(gameX / 850 + 1));
//   * the row block counts *down* from the map origin, so the row inside a tile is
//     map y plus a whole tile per block index (the game's tileY = ceil(-gameY / 850)).
// Row order is what makes this worth spelling out: reading a tile the way a mosaic of the
// map image would put it lands on the vertically mirrored piece of terrain.  The measured
// evidence is in MEMORY: the archive's own seams continue under this rule and not under
// the mirrored one, and the shipped feature packs - which localise correctly in game -
// are built with it.
struct GridAxis {
    int tile = 0;
    int pixel = 0;
};

GridAxis ColumnOf(double mapX) {
    const int tile = static_cast<int>(std::floor(mapX / kTileSize)) + 1;
    return {tile, static_cast<int>(std::floor(mapX)) + kTileSize - kTileSize * tile};
}

GridAxis RowOf(double mapY) {
    const int tile = static_cast<int>(std::ceil(-mapY / kTileSize));
    return {tile, static_cast<int>(std::floor(mapY)) + kTileSize * tile};
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

    // Map pixels per minimap pixel: the tiles are 1024 px per 850 world units and the
    // normalised minimap covers kNormalizedMinimapSize * terrainScale world units.
    const double scale = kTilePixelsPerUnit * terrainScale;
    const int templateWidth = static_cast<int>(std::lround(normalizedMinimap.cols * scale));
    const int templateHeight = static_cast<int>(std::lround(normalizedMinimap.rows * scale));
    if (templateWidth < 32 || templateHeight < 32) return finish("reason=template-too-small");

    // The prior arrives as a map-image coordinate (scene->scale * world + scene->origin),
    // the space VisualLocalizationCandidate::mapCenter uses; the archive is indexed in map
    // pixels.
    const double mapX = kTilePixelsPerUnit * (prior.x - scene->originX) / scene->scale;
    const double mapY = kTilePixelsPerUnit * (prior.y - scene->originY) / scene->scale;

    // The reference window, centred on the prior, in map pixels.  Each row and column of
    // the window lands on its own tile and pixel; resolving them once keeps the copy below
    // a straight read and touches at most four tiles.
    const int side = std::max(templateWidth, templateHeight) + 2 * kSearchRadius;
    std::vector<GridAxis> rows(side), columns(side);
    for (int j = 0; j < side; ++j) rows[j] = RowOf(mapY - side / 2.0 + j);
    for (int i = 0; i < side; ++i) columns[i] = ColumnOf(mapX - side / 2.0 + i);

    cv::Mat window = cv::Mat::zeros(side, side, CV_8U);
    long long covered = 0;
    int loadedX = std::numeric_limits<int>::min(), loadedY = std::numeric_limits<int>::min();
    cv::Mat tile;
    for (int j = 0; j < side; ++j) {
        uchar* destination = window.ptr<uchar>(j);
        for (int i = 0; i < side; ++i) {
            const auto& column = columns[i];
            const auto& row = rows[j];
            if (column.tile != loadedX || row.tile != loadedY) {
                tile = LoadTile(referenceRoot, scene->kuroStateId, column.tile, row.tile);
                loadedX = column.tile;
                loadedY = row.tile;
            }
            if (tile.empty()) continue;
            destination[i] = tile.at<uchar>(row.pixel, column.pixel);
            ++covered;
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
    // The window is centred on the prior, so the cell whose template centre is the prior
    // is the correlation *at the prior*: that is the number the decision uses, because the
    // position is trusted from the prior and this only measures how well the map agrees
    // there.  The peak is recorded for diagnosis and deliberately not used as a position -
    // it wanders by tens of pixels on a frame whose prior is already right.
    const int priorX = std::max(0, std::min(score.cols - 1, (side - templateWidth) / 2));
    const int priorY = std::max(0, std::min(score.rows - 1, (side - templateHeight) / 2));
    result.available = true;
    result.score = score.at<float>(priorY, priorX);
    result.peakScore = peak;
    result.peakOffsetX = peakAt.x - priorX;
    result.peakOffsetY = peakAt.y - priorY;
    result.accepted = result.score >= MinimumScore;
    return finish(Numbers(result.score, result.peakScore, result.peakOffsetX, result.peakOffsetY));
}
}
