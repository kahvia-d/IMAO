#include "MatcherBench.h"

#include <opencv2/calib3d.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/xfeatures2d.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

#include "../../src/App/MapViewportCandidates.h"
#include "../../src/App/MapViewportGeometry.h"

namespace MatcherBench {

using namespace MapViewportCandidates;

struct Sample {
    int ordinal = 0;
    cv::Point2f truth;              // where the query came from, in query pixels
    int cropKeypoints = 0;
    // Shipped path: a de-duplicated candidate copy plus a FLANN index over it.
    double flannBuildMs = 0.0;
    double flannMatchMs = 0.0;
    std::size_t flannCandidates = 0;
    int flannRaw = 0;
    int flannGood = 0;
    int flannInliers = 0;
    double flannIdentity = 0.0;
    // Alternative: the map rows themselves, pairs de-duplicated after matching.
    double exactMatchMs = 0.0;
    std::size_t exactRows = 0;
    int exactRaw = 0;
    int exactGood = 0;
    int exactInliers = 0;
    double exactIdentity = 0.0;
};

inline double Milliseconds(const std::chrono::steady_clock::time_point& start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// Every enabled row that belongs to the scene's tiles - the row set both matchers are given, so the only
// difference between the measurements below is how each one compares a query against those rows.
std::vector<std::uint32_t> SceneRows(const RuntimeFeatureResources& resources, int sceneId) {
    std::vector<std::uint32_t> rows;
    std::vector<unsigned char> selected(resources.map.imgKeypoints.size(), 0);
    for (std::size_t tileIndex = 0; tileIndex < resources.visualIndex.tiles.size(); ++tileIndex) {
        if (TileScene(resources, tileIndex) != sceneId) continue;
        const auto& tile = resources.visualIndex.tiles[tileIndex];
        const std::size_t first = tile.featureRowOffset;
        const std::size_t last = first + tile.featureRowCount;
        if (last > resources.visualIndex.featureRows.size()) continue;
        for (std::size_t index = first; index < last; ++index) {
            const auto row = resources.visualIndex.featureRows[index];
            if (row < selected.size()) selected[row] = 1;
        }
    }
    for (std::size_t row = 0; row < selected.size(); ++row) {
        if (selected[row] && resources.FeatureRowEnabled(row)) rows.push_back(static_cast<std::uint32_t>(row));
    }
    return rows;
}

// Points that describe the same physical place, compared by value rather than by address.
struct CoordinateIdentity {
    std::uint64_t Key(const cv::Point2f& point) const {
        return (static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(point.x)) << 32) |
            std::bit_cast<std::uint32_t>(point.y);
    }
    std::unordered_set<std::uint64_t> seen;
    bool Insert(const cv::Point2f& point) { return seen.insert(Key(point)).second; }
};

// How close the recovered transform is to mapping the query onto itself. A tile matched against its own
// scene has to come back as the identity, so this is a correctness check that needs no coordinate
// conversion: it does not care what units the map uses.
double IdentityScore(const cv::Mat& transform, const cv::Size& cropSize) {
    if (transform.empty() || transform.rows < 3) return 0.0;
    const std::vector<cv::Point2f> corners{
        { 0.0f, 0.0f }, { static_cast<float>(cropSize.width), 0.0f },
        { static_cast<float>(cropSize.width), static_cast<float>(cropSize.height) },
        { 0.0f, static_cast<float>(cropSize.height) } };
    std::vector<cv::Point2f> mapped;
    cv::perspectiveTransform(corners, mapped, transform);
    if (mapped.size() != corners.size()) return 0.0;
    double worst = 0.0;
    for (std::size_t index = 0; index < corners.size(); ++index)
        worst = std::max<double>(worst, cv::norm(mapped[index] - corners[index]));
    const double diagonal = std::hypot(static_cast<double>(cropSize.width), static_cast<double>(cropSize.height));
    return std::max(0.0, 1.0 - worst / diagonal);
}

// The shipped path: one owned, de-duplicated candidate copy, a FLANN index trained over it, then the same
// ratio test, geometry fit and identity check the alternative gets.
void RunFlann(const RuntimeFeatureResources& resources, const std::vector<std::uint32_t>& tileIndices,
    const ImageFeatureData& queryFeatures, const cv::Mat& crop, Sample& sample) {
    const auto buildStart = std::chrono::steady_clock::now();
    ImageFeatureData candidates = SelectSceneCandidates(resources, tileIndices);
    cv::Ptr<cv::FlannBasedMatcher> matcher;
    if (!candidates.imgDescriptors.empty()) {
        matcher = cv::makePtr<cv::FlannBasedMatcher>();
        matcher->add(std::vector<cv::Mat>{ candidates.imgDescriptors });
        matcher->train();
    }
    sample.flannBuildMs = Milliseconds(buildStart);
    sample.flannCandidates = candidates.imgDescriptors.rows;
    if (!matcher || queryFeatures.imgDescriptors.empty()) return;

    const auto matchStart = std::chrono::steady_clock::now();
    std::vector<std::vector<cv::DMatch>> pairs;
    matcher->knnMatch(queryFeatures.imgDescriptors, pairs, 2);
    const auto raw = FilterGoodMatches(pairs);
    sample.flannMatchMs = Milliseconds(matchStart);
    sample.flannRaw = static_cast<int>(raw.size());

    CoordinateIdentity identity;
    std::vector<cv::Point2f> cropPoints, mapPoints;
    for (const auto& match : raw) {
        if (!identity.Insert(candidates.imgKeypoints[match.trainIdx].pt)) continue;
        cropPoints.push_back(queryFeatures.imgKeypoints[match.queryIdx].pt);
        mapPoints.push_back(candidates.imgKeypoints[match.trainIdx].pt);
    }
    sample.flannGood = static_cast<int>(cropPoints.size());
    if (cropPoints.size() < 12) return;

    cv::Mat inliers;
    const cv::Mat transform = FitMapViewportTransform(cropPoints, mapPoints, inliers);
    if (transform.empty() || inliers.empty()) return;
    sample.flannInliers = cv::countNonZero(inliers);
    sample.flannIdentity = IdentityScore(transform, crop.size());
}

// The alternative: match the map rows directly, keep the pairs that pass the same ratio test, remove pairs
// that describe the same physical point, then fit. No copy, no index, nothing cached between searches.
void RunExact(const RuntimeFeatureResources& resources, const std::vector<std::uint32_t>& rows,
    const ImageFeatureData& queryFeatures, const cv::Mat& crop, Sample& sample) {
    sample.exactRows = rows.size();
    if (rows.empty() || queryFeatures.imgDescriptors.empty()) return;

    const auto matchStart = std::chrono::steady_clock::now();
    const auto matched = MatchRowsExactly(resources, rows, queryFeatures.imgDescriptors);
    sample.exactMatchMs = Milliseconds(matchStart);
    sample.exactRaw = static_cast<int>(matched.size());

    CoordinateIdentity identity;
    std::vector<cv::Point2f> cropPoints, mapPoints;
    for (const auto& match : matched) {
        if (match.trainIdx < 0 || match.trainIdx >= static_cast<int>(rows.size())) continue;
        const auto row = rows[static_cast<std::size_t>(match.trainIdx)];
        if (!identity.Insert(resources.map.imgKeypoints[row].pt)) continue;
        cropPoints.push_back(queryFeatures.imgKeypoints[match.queryIdx].pt);
        mapPoints.push_back(resources.map.imgKeypoints[row].pt);
    }
    sample.exactGood = static_cast<int>(cropPoints.size());
    if (cropPoints.size() < 12) return;

    cv::Mat inliers;
    const cv::Mat transform = FitMapViewportTransform(cropPoints, mapPoints, inliers);
    if (transform.empty() || inliers.empty()) return;
    sample.exactInliers = cv::countNonZero(inliers);
    sample.exactIdentity = IdentityScore(transform, crop.size());
}

// Queries taken from the scene's own rendered map tiles: the exact pixels the map features were extracted
// from, so scale and style are right by construction and the identity check is meaningful. The row set is
// the whole scene, which is the strongest form of the comparison - the shipped path cannot widen further.
int RunSceneQueries(const RuntimeFeatureResources& resources, const nlohmann::json& config,
    nlohmann::json& report) {
    const int sceneId = config.at("sceneId").get<int>();
    const std::filesystem::path tileDirectory = config.at("tileDirectory").get<std::string>();
    const int maximumSamples = config.value("maximumSamples", 8);
    const int querySize = config.value("querySize", 512);
    const int stride = config.value("stride", 512);

    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(tileDirectory)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".png") continue;
        names.push_back(entry.path().string());
    }
    std::sort(names.begin(), names.end());

    const auto rows = SceneRows(resources, sceneId);
    std::cerr << "scene queries: scene=" << sceneId << " images=" << names.size()
        << " rows=" << rows.size() << " query=" << querySize << " stride=" << stride << '\n';
    if (rows.empty()) {
        std::cerr << "scene queries: no rows for that scene - is the pack installed in this asset root?\n";
        return 1;
    }

    auto surf = cv::xfeatures2d::SURF::create(100, 4, 3, true, true);
    std::vector<Sample> samples;
    for (const auto& path : names) {
        if (static_cast<int>(samples.size()) >= maximumSamples) break;
        const cv::Mat image = cv::imread(path, cv::IMREAD_GRAYSCALE);
        if (image.empty() || image.cols < querySize || image.rows < querySize) continue;
        const cv::Mat crop = image(cv::Rect((image.cols - querySize) / 2, (image.rows - querySize) / 2,
            querySize, querySize)).clone();
        ImageFeatureData queryFeatures = FeatureMatch::ExtractSurfFeatures(surf, crop);
        if (queryFeatures.imgDescriptors.empty() || queryFeatures.imgKeypoints.size() < 12) continue;

        Sample sample;
        sample.ordinal = static_cast<int>(samples.size());
        sample.truth = cv::Point2f(crop.cols / 2.0f, crop.rows / 2.0f);
        sample.cropKeypoints = static_cast<int>(queryFeatures.imgKeypoints.size());
        std::cerr << "  query " << sample.ordinal << " " << std::filesystem::path(path).filename().string()
            << " keypoints=" << sample.cropKeypoints << std::flush;

        // One window around the query, chosen the way the shipped path chooses one. The whole-scene row set
        // above is the fallback for a query whose window cannot be located - it is still the same rows for
        // both matchers, so the comparison stays honest.
        const auto tileIndices = SelectSceneTileIndices(resources, sceneId, std::nullopt);
        RunFlann(resources, tileIndices, queryFeatures, crop, sample);
        RunExact(resources, rows, queryFeatures, crop, sample);
        std::cerr << " flann(build=" << sample.flannBuildMs << "ms match=" << sample.flannMatchMs
            << "ms cand=" << sample.flannCandidates << " raw=" << sample.flannRaw << " good=" << sample.flannGood
            << " inl=" << sample.flannInliers << " id=" << sample.flannIdentity << ")"
            << " exact(match=" << sample.exactMatchMs << "ms rows=" << sample.exactRows
            << " raw=" << sample.exactRaw << " good=" << sample.exactGood
            << " inl=" << sample.exactInliers << " id=" << sample.exactIdentity << ")\n";
        samples.push_back(sample);
    }

    report["sceneId"] = sceneId;
    report["sceneRows"] = rows.size();
    report["sampleCount"] = samples.size();
    report["samples"] = nlohmann::json::array();
    for (const auto& sample : samples) {
        report["samples"].push_back({
            { "ordinal", sample.ordinal },
            { "truthX", sample.truth.x }, { "truthY", sample.truth.y },
            { "cropKeypoints", sample.cropKeypoints },
            { "flannCandidates", sample.flannCandidates },
            { "flannBuildMs", sample.flannBuildMs }, { "flannMatchMs", sample.flannMatchMs },
            { "flannRaw", sample.flannRaw }, { "flannGood", sample.flannGood },
            { "flannInliers", sample.flannInliers }, { "flannIdentity", sample.flannIdentity },
            { "exactRows", sample.exactRows },
            { "exactMatchMs", sample.exactMatchMs },
            { "exactRaw", sample.exactRaw }, { "exactGood", sample.exactGood },
            { "exactInliers", sample.exactInliers }, { "exactIdentity", sample.exactIdentity },
        });
    }
    return 0;
}

// A staged measurement: run one query at one scale through both matchers and print the pair counts. Used
// to find the scale the map features were extracted at, which is not recorded anywhere and cannot be
// guessed - SURF descriptors only match at the scale they were computed at. The scale that lights up is
// the answer, and it is the only way to get queries and features onto the same footing (2026-10-02).
int RunScaleSweep(const RuntimeFeatureResources& resources, const nlohmann::json& config,
    nlohmann::json& report) {
    const int sceneId = config.at("sceneId").get<int>();
    const std::string imagePath = config.at("image").get<std::string>();
    const int querySize = config.value("querySize", 512);
    const std::vector<double> scales = config.value("scales", std::vector<double>{ 0.25, 0.4, 0.5, 0.75, 1.0, 1.5, 2.0 });

    const cv::Mat image = cv::imread(imagePath, cv::IMREAD_GRAYSCALE);
    if (image.empty() || image.cols < querySize || image.rows < querySize) {
        std::cerr << "scale sweep cannot read a usable image at " << imagePath << '\n';
        return 1;
    }
    const cv::Mat crop = image(cv::Rect((image.cols - querySize) / 2, (image.rows - querySize) / 2,
        querySize, querySize)).clone();
    const auto rows = SceneRows(resources, sceneId);
    std::cerr << "scale sweep: image=" << image.cols << "x" << image.rows << " crop=" << querySize
        << " rows=" << rows.size() << '\n';

    auto surf = cv::xfeatures2d::SURF::create(100, 4, 3, true, true);
    report["sceneRows"] = rows.size();
    report["samples"] = nlohmann::json::array();
    for (const double scale : scales) {
        cv::Mat scaled = crop;
        if (std::abs(scale - 1.0) > 1e-6) {
            cv::resize(crop, scaled, {}, scale, scale, scale > 1.0 ? cv::INTER_CUBIC : cv::INTER_AREA);
        }
        ImageFeatureData queryFeatures = FeatureMatch::ExtractSurfFeatures(surf, scaled);
        nlohmann::json entry;
        entry["scale"] = scale;
        entry["queryKeypoints"] = queryFeatures.imgKeypoints.size();
        if (queryFeatures.imgDescriptors.empty()) {
            entry["note"] = "no descriptors";
            report["samples"].push_back(entry);
            std::cerr << "  scale " << scale << " keypoints=0\n";
            continue;
        }

        Sample sample;
        sample.exactRows = rows.size();
        const auto matchStart = std::chrono::steady_clock::now();
        const auto matched = MatchRowsExactly(resources, rows, queryFeatures.imgDescriptors);
        sample.exactMatchMs = Milliseconds(matchStart);
        sample.exactRaw = static_cast<int>(matched.size());
        const auto tileIndices = SelectSceneTileIndices(resources, sceneId, std::nullopt);
        ImageFeatureData candidates = SelectSceneCandidates(resources, tileIndices);
        sample.flannCandidates = candidates.imgDescriptors.rows;
        if (!candidates.imgDescriptors.empty()) {
            auto matcher = cv::makePtr<cv::FlannBasedMatcher>();
            matcher->add(std::vector<cv::Mat>{ candidates.imgDescriptors });
            matcher->train();
            std::vector<std::vector<cv::DMatch>> pairs;
            matcher->knnMatch(queryFeatures.imgDescriptors, pairs, 2);
            sample.flannRaw = static_cast<int>(FilterGoodMatches(pairs).size());
        }
        entry["rows"] = rows.size();
        entry["exactRaw"] = sample.exactRaw;
        entry["exactMatchMs"] = sample.exactMatchMs;
        entry["flannCandidates"] = sample.flannCandidates;
        entry["flannRaw"] = sample.flannRaw;
        report["samples"].push_back(entry);
        std::cerr << "  scale " << scale << " keypoints=" << entry["queryKeypoints"]
            << " exactRaw=" << sample.exactRaw << " flannRaw=" << sample.flannRaw
            << " exactMs=" << sample.exactMatchMs << '\n';
    }
    report["sampleCount"] = report["samples"].size();
    return 0;
}

int Run(const RuntimeFeatureResources& resources, const std::filesystem::path& repositoryRoot,
    const nlohmann::json& config, nlohmann::json& report) {
    (void)repositoryRoot;
    if (config.contains("scales")) return RunScaleSweep(resources, config, report);
    if (config.contains("tileDirectory")) return RunSceneQueries(resources, config, report);
    std::cerr << "matcher bench needs tileDirectory or scales\n";
    return 1;
}

}   // namespace MatcherBench
