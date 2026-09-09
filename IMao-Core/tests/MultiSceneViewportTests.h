#pragma once

#include "App/MapViewportLocalizer.h"
#include "Feature/Match/SceneMapFeatures.h"
#include "Coordinate/VisualLocalization/GlobalVisualLocalizer.h"
#include <opencv2/imgproc.hpp>
#include <opencv2/xfeatures2d.hpp>
#include <chrono>
#include <iostream>
#include <thread>

// Synthetic scene identities intentionally occupy identical map coordinates.
// This tests worker routing and geometry, not real-game map calibration.
inline int RunMultiSceneViewportTests() {
    Scene::EnsureExternalConfigLoaded();
    const auto savedMinimapScales = Scene::minimapScales;
    Scene::minimapScales.clear(); // Synthetic fixtures have their own scale.
    int failures = 0;
    auto expect = [&](bool passed, const char* name) {
        std::cout << (passed ? "PASS " : "FAIL ") << name << '\n';
        if (!passed) ++failures;
    };
    auto resources = std::make_shared<RuntimeFeatureResources>();
    resources->visualIndexReady = true;
    resources->baseVisualTileCount = 1;
    std::vector<cv::Mat> images;
    auto add = [&](int scene, const cv::Mat& image) {
        auto surf = cv::xfeatures2d::SURF::create(100, 4, 3, true, true);
        auto features = FeatureMatch::ExtractSurfFeatures(surf, image);
        MapVisualTile tile;
        tile.sceneId = scene;
        tile.minX = tile.minY = 100;
        tile.maxX = tile.maxY = 500;
        tile.featureRowOffset = static_cast<std::uint32_t>(resources->visualIndex.featureRows.size());
        tile.featureRowCount = static_cast<std::uint32_t>(features.imgKeypoints.size());
        for (std::size_t i = 0; i < features.imgKeypoints.size(); ++i) {
            auto point = features.imgKeypoints[i];
            point.pt += cv::Point2f(100, 100);
            resources->visualIndex.featureRows.push_back(static_cast<std::uint32_t>(resources->map.imgKeypoints.size()));
            resources->map.imgKeypoints.push_back(point);
            resources->map.imgDescriptors.push_back(features.imgDescriptors.row(static_cast<int>(i)));
        }
        resources->visualIndex.tiles.push_back(tile);
    };
    for (int id = 1; id <= 5; ++id) {
        cv::Mat image(400, 400, CV_8UC3);
        cv::RNG rng(9017 + id);
        rng.fill(image, cv::RNG::UNIFORM, 0, 256);
        cv::GaussianBlur(image, image, {5, 5}, 1.0);
        images.push_back(image);
        // Legacy base tiles can carry historical non-World partition IDs.
        add(id == 1 ? 2 : id, image);
    }
    cv::Mat gatedImage(400, 400, CV_8UC3);
    cv::RNG gatedRng(19009);
    gatedRng.fill(gatedImage, cv::RNG::UNIFORM, 0, 256);
    cv::GaussianBlur(gatedImage, gatedImage, {5, 5}, 1.0);
    add(6, gatedImage);
    for (int scene = 1; scene <= 5; ++scene) {
        const auto nearby = SceneMapFeaturesNear(*resources, scene, Coordinate(300, 300), 300);
        expect(nearby.imgKeypoints.size() == resources->visualIndex.tiles[scene - 1].featureRowCount,
            "OCR and map-open feature windows exclude overlapping scenes");
    }
    expect(SceneMapFeaturesNear(*resources, 6, Coordinate(300, 300), 300).imgDescriptors.empty(),
        "OCR cannot use an unapproved scene");
    std::string error;
    expect(MapViewportLocalizer::Initialize(resources, error), "worker initializes");
    std::uint64_t serial = 0;
    auto locate = [&](const cv::Mat& image, int priorScene = 0, double radius = 512.0) {
        MapViewportLocalizationRequest request;
        request.requestId = ++serial;
        request.mapCrop = image;
        if (priorScene) {
            request.scope = MapViewportSearchScope::Local512;
            WorldSearchPrior prior;
            prior.valid = true; prior.sceneId = priorScene;
            prior.centerMapCoordinate = radius == 1.0 ? Coordinate(5000, 5000) : Coordinate(300, 300);
            prior.radius = radius;
            request.prior = prior;
        }
        MapViewportLocalizer::Submit(std::move(request));
        MapViewportLocalizationResult result;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < deadline) {
            if (MapViewportLocalizer::TryTakeLatestResult(result) && result.requestId == serial) return result;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        expect(false, "worker returns before deadline");
        return result;
    };
    for (int scene = 1; scene <= 5; ++scene) {
        auto result = locate(images[scene - 1]);
        expect(result.accepted && result.sceneId == scene &&
            std::hypot(result.centerMapCoordinate.x - 300, result.centerMapCoordinate.y - 300) < 1,
            "global search preserves matched scene despite overlapping coordinates");
        result = locate(images[scene - 1], scene);
        expect(result.accepted && result.sceneId == scene, "local search preserves scene");
    }
    expect(!locate(gatedImage).accepted, "unapproved scene is excluded even when features exist");
    expect(!locate(images[1], 1).accepted, "wrong scene prior cannot match another scene");
    expect(!locate(images[1], 2, 1.0).accepted, "empty local coverage never falls back to all features");
    expect(!locate(cv::Mat::zeros(400, 400, CV_8UC3)).accepted, "blank map rejected");
    MapViewportLocalizer::Shutdown();
    resources->visualIndex.vocabulary = resources->map.imgDescriptors.rowRange(0, 64).clone();
    resources->visualIndex.postingOffsets.assign(MapVisualIndex::WordCount + 1, 0);
    for (std::uint32_t i = 1; i < 5; ++i) resources->kuroVisualShards.push_back({static_cast<int>(i + 1), i, 1});
    auto diagnose = [&](const cv::Mat& image) {
        VisualLocalizationRequest request;
        request.normalizedMinimap = image;
        auto surf = cv::xfeatures2d::SURF::create(100, 4, 3, true, true);
        request.minimapFeatures = FeatureMatch::ExtractSurfFeatures(surf, image);
        return GlobalVisualLocalizer::LocateForDiagnostics(resources, request);
    };
    auto recovered = diagnose(images[1]);
    expect(recovered.quality != VisualLocalizationQuality::Rejected && !recovered.candidates.empty() &&
        recovered.candidates.front().sceneId == 2, "minimap fallback searches independent shards");
    expect(GlobalVisualLocalizer::Initialize(resources, error), "minimap engine initializes");
    for (int scene = 1; scene <= 5; ++scene) {
        auto surf = cv::xfeatures2d::SURF::create(100, 4, 3, true, true);
        auto features = FeatureMatch::ExtractSurfFeatures(surf, images[scene - 1]);
        VisualLocalizationCandidate result;
        expect(GlobalVisualLocalizer::TrackLocal(images[scene - 1], features, scene,
            Coordinate(300, 300), result, 1.0) && result.sceneId == scene,
            "minimap tracking preserves independent scene");
        if (scene == 1) {
            expect(!GlobalVisualLocalizer::TrackLocal(images[0], features, 2,
                Coordinate(300, 300), result, 1.0),
                "historical World partition cannot leak into Tethys tracking");
        }
    }
    GlobalVisualLocalizer::Shutdown();
    {
        auto doubled = std::make_shared<RuntimeFeatureResources>(*resources);
        auto& tile = doubled->visualIndex.tiles[4];
        tile.maxX = tile.maxY = 900;
        for (std::uint32_t i = 0; i < tile.featureRowCount; ++i) {
            const auto row = doubled->visualIndex.featureRows[tile.featureRowOffset + i];
            auto& point = doubled->map.imgKeypoints[row].pt;
            point = cv::Point2f(100,100) + (point - cv::Point2f(100,100)) * 2.0f;
        }
        Scene::minimapScales[5] = 2.0;
        expect(GlobalVisualLocalizer::Initialize(doubled, error), "scene-scale worker initializes");
        auto surf = cv::xfeatures2d::SURF::create(100, 4, 3, true, true);
        auto features = FeatureMatch::ExtractSurfFeatures(surf, images[4]);
        VisualLocalizationCandidate scaled;
        expect(GlobalVisualLocalizer::TrackLocal(images[4], features, 5, Coordinate(500,500), scaled) &&
            std::abs(scaled.scale - 2.0) < 0.01, "scene-specific double minimap scale tracks without wider gates");
        expect(!GlobalVisualLocalizer::TrackLocal(images[4], features, 5, Coordinate(500,500), scaled, 1.0),
            "World scale is rejected in a double-scale scene");
        GlobalVisualLocalizer::Shutdown();
        Scene::minimapScales.erase(5);
    }
    // A replacement retires legacy duplicates across every matching path.
    add(2, images[0]);
    resources->kuroVisualShards.push_back({2, static_cast<std::uint32_t>(resources->visualIndex.tiles.size() - 1), 1});
    resources->excludedBaseRows.assign(resources->visualIndex.tiles[0].featureRowCount, 1);
    expect(SceneMapFeaturesNear(*resources, 1, Coordinate(300, 300), 300).imgDescriptors.empty(),
        "retired legacy rows cannot enter OCR windows");
    expect(MapViewportLocalizer::Initialize(resources, error), "replacement worker initializes");
    auto replacement = locate(images[0]);
    expect(replacement.accepted && replacement.sceneId == 2,
        "replacement scene wins only after duplicate legacy rows are retired");
    MapViewportLocalizer::Shutdown();
    auto replacementMinimap = diagnose(images[0]);
    expect(!replacementMinimap.candidates.empty() && replacementMinimap.candidates.front().sceneId == 2,
        "minimap replacement retains explicit scene identity");
    // Give a second scene the exact same image: neither identity is defensible.
    add(3, images[1]);
    resources->kuroVisualShards.push_back({3, static_cast<std::uint32_t>(resources->visualIndex.tiles.size() - 1), 1});
    const auto ambiguousMinimap = diagnose(images[1]);
    expect(ambiguousMinimap.ambiguous && ambiguousMinimap.quality == VisualLocalizationQuality::Rejected,
        "minimap identical coordinates in different scenes are ambiguous");
    expect(MapViewportLocalizer::Initialize(resources, error), "worker reinitializes");
    auto ambiguous = locate(images[1]);
    expect(!ambiguous.accepted && ambiguous.sceneId == 0, "ambiguous scenes rejected");
    MapViewportLocalizer::Shutdown();
    Scene::minimapScales = savedMinimapScales;
    return failures == 0 ? 0 : 3;
}
