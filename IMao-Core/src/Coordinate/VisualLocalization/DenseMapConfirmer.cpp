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
struct CachedTile {
    cv::Mat image;
    std::uint64_t lastUsed = 0;
};
std::unordered_map<std::string, CachedTile> tileCache;
std::uint64_t cacheClock = 0;
// A window touches at most four tiles, and a decode costs about as much as the
// correlation itself, so the cache has to survive the player walking: the first version
// wiped all twelve entries on overflow, which made almost every confirmation cold again.
constexpr std::size_t kCacheLimit = 48;

std::string TileKey(int state, int x, int y) {
    return std::to_string(state) + ":" + std::to_string(x) + ":" + std::to_string(y);
}

// Decoded tiles are 1 MB each, so a modest cache turns the per-frame cost into the
// correlation alone.  Least recently used goes first.
cv::Mat LoadTile(const std::filesystem::path& root, int state, int x, int y) {
    const auto key = TileKey(state, x, y);
    {
        std::scoped_lock lock(cacheMutex);
        const auto found = tileCache.find(key);
        if (found != tileCache.end()) {
            found->second.lastUsed = ++cacheClock;
            return found->second.image;
        }
    }
    const auto path = root / std::to_string(state) /
        (std::to_string(state) + "_" + std::to_string(x) + "_" + std::to_string(y) + ".png");
    cv::Mat loaded = cv::imread(path.string(), cv::IMREAD_GRAYSCALE);
    if (!loaded.empty() && loaded.type() != CV_8U) loaded.convertTo(loaded, CV_8U);
    {
        std::scoped_lock lock(cacheMutex);
        if (tileCache.size() >= kCacheLimit) {
            auto oldest = tileCache.begin();
            for (auto entry = tileCache.begin(); entry != tileCache.end(); ++entry) {
                if (entry->second.lastUsed < oldest->second.lastUsed) oldest = entry;
            }
            tileCache.erase(oldest);
        }
        tileCache[key] = CachedTile{ loaded, ++cacheClock };
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

// The localizer hands over the minimap it normalised, which is a colour crop resized to
// 184 square - not the grey it extracts features from.  Convert it the same way the
// feature path does, so both see the same picture.
cv::Mat ToGray(const cv::Mat& image) {
    cv::Mat gray;
    if (image.channels() == 1) gray = image;
    else if (image.channels() == 4) cv::cvtColor(image, gray, cv::COLOR_BGRA2GRAY);
    else cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    return gray;
}

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
    if (normalizedMinimap.empty() || !(terrainScale > 0.0) ||
        !std::isfinite(prior.x) || !std::isfinite(prior.y)) return finish("reason=input-unusable");
    const auto* scene = Scene::Find(sceneId);
    if (scene == nullptr || scene->kuroStateId <= 0) return finish("reason=unknown-scene");
    if (referenceRoot.empty()) return finish("reason=no-reference");
    // Note the channel count here: the localizer's normalised minimap is a colour image,
    // and refusing it as "unusable" made a whole release of this confirmer never run once.
    if (normalizedMinimap.channels() != 1 && normalizedMinimap.channels() != 3 &&
        normalizedMinimap.channels() != 4) return finish("reason=input-unusable");
    cv::Mat gray = ToGray(normalizedMinimap);
    if (gray.depth() != CV_8U) {
        cv::Mat converted;
        gray.convertTo(converted, CV_8U);
        gray = converted;
    }
    if (gray.empty()) return finish("reason=input-unusable");

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
    // At most two tiles per axis intersect a window this size, so load them up front:
    // four lookups instead of one per pixel.
    const int columnTiles[2] = {columns[0].tile, columns[side - 1].tile};
    const int rowTiles[2] = {rows[0].tile, rows[side - 1].tile};
    cv::Mat tiles[2][2];
    for (int a = 0; a < 2; ++a) {
        for (int b = 0; b < 2; ++b) {
            tiles[a][b] = LoadTile(referenceRoot, scene->kuroStateId, columnTiles[a], rowTiles[b]);
        }
    }
    for (int j = 0; j < side; ++j) {
        const auto& row = rows[j];
        const int b = row.tile == rowTiles[0] ? 0 : 1;
        uchar* destination = window.ptr<uchar>(j);
        int current = -1;
        const uchar* source = nullptr;
        for (int i = 0; i < side; ++i) {
            const auto& column = columns[i];
            const int a = column.tile == columnTiles[0] ? 0 : 1;
            if (a != current) {
                current = a;
                const cv::Mat& tile = tiles[a][b];
                source = tile.empty() || row.pixel >= tile.rows ? nullptr : tile.ptr<uchar>(row.pixel);
            }
            if (source == nullptr) continue;
            destination[i] = source[column.pixel];
            ++covered;
        }
    }
    if (covered < kMinimumCoverage * side * side) return finish("reason=reference-incomplete");

    // The template: the minimap at map scale, masked to the ring the runtime trusts.
    cv::Mat templateImage;
    cv::resize(gray, templateImage, cv::Size(templateWidth, templateHeight), 0.0, 0.0, cv::INTER_LINEAR);
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
