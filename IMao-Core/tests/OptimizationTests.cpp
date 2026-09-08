#include "CoarseSearchCoverageTests.h"
#include "OverlayVisibilityTests.h"
#include "GamepadContextTests.h"
#include "GamepadCursorTargetsTests.h"
#include "GamepadWorldActionsTests.h"
#include "RouteGamepadTests.h"
#include "MapToolsTests.h"
#include "OverlayPanelLayoutTests.h"
#include "MapControllerUiTests.h"
#include "OverlayMotionTests.h"
#include "ScreenMotionTrackerTests.h"
#include "MinimapTrackingGeometryTests.h"
#include "ImageAnchoredOverlayTests.h"
#include "MinimapHudEvidenceTests.h"
#include "Runtime/SnapshotChannel.h"
#include "Runtime/FrameState.h"
#include "Feature/Match/ExactDescriptorMatcher.h"
#include "Feature/Match/FeatureRowCache.h"
#include "Coordinate/KuroMapCoordinates.h"
#include "Runtime/SceneItemStore.h"
#include "Runtime/UserFileName.h"
#include "Runtime/AtomicFile.h"
#include <thread>
#include "Coordinate/IdentifyWorldCoordinates/CoordinateCandidate.h"
#include "Coordinate/IdentifyWorldCoordinates/CoordinateRecoveryController.h"
#include "App/MapUiStateController.h"
#include "App/MapUiVisualDetector.h"
#include "App/MapViewportPredictor.h"
#include "App/MapViewportGeometry.h"
#include "App/MinimapResumePolicy.h"
#include "Coordinate/VisualLocalization/RecoveryPolicy.h"
#include "App/WorldSearchPrior.h"
#include "Feature/Processing/FeatureBinaryCodec.h"
#include "Feature/VisualIndex/MapVisualIndex.h"
#include "Feature/Match/UniqueMapFeatures.h"

#include <Windows.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <wil/resource.h>

namespace {
int failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::vector<std::uint8_t> ReadAll(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
}

void WriteAll(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void WriteLittleEndian32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>((value >> (index * 8)) & 0xff);
    }
}

void TestExactDescriptorMatching() {
    cv::Mat map(8300, 128, CV_32F), query(36, 128, CV_32F);
    cv::RNG random(1907);
    random.fill(map, cv::RNG::UNIFORM, 0.0f, 1.0f);
    random.fill(query, cv::RNG::UNIFORM, 0.0f, 1.0f);
    map.row(0).copyTo(map.row(4096)); // Stable tie behaviour across batches.
    query.row(0).copyTo(query.row(1));
    std::vector<std::vector<cv::DMatch>> forward, reverse, expectedForward, expectedReverse;
    MatchDescriptorsExactly(map, query, forward, reverse);
    cv::BFMatcher matcher(cv::NORM_L2);
    matcher.knnMatch(map, query, expectedForward, 2);
    matcher.knnMatch(query, map, expectedReverse, 1);
    for (size_t row = 0; row < forward.size(); ++row) for (size_t match = 0; match < 2; ++match)
        Expect(forward[row][match].trainIdx == expectedForward[row][match].trainIdx &&
            std::abs(forward[row][match].distance - expectedForward[row][match].distance) < 0.00001f,
            "batched exact forward matches preserve BF neighbour identities and distances");
    for (size_t row = 0; row < reverse.size(); ++row)
        Expect(reverse[row][0].trainIdx == expectedReverse[row][0].trainIdx,
            "batched exact reverse matches preserve BF mutual-neighbour identity");
    int checks = 0;
    bool stopped = false;
    try { MatchDescriptorsExactly(map, query, forward, reverse, [&] { return ++checks == 2; }); }
    catch (const SearchInterrupted&) { stopped = true; }
    Expect(stopped && checks == 2, "exact matching can be cancelled between batches");
    ImageFeatureData source;
    source.imgDescriptors = map;
    for (int row = 0; row < map.rows; ++row) source.imgKeypoints.emplace_back(float(row), float(row), 1.0f);
    source.imgKeypoints[4096] = source.imgKeypoints[0];
    FeatureRowCache cache(source, 2048);
    std::vector<uint32_t> rows{0, 4096, 1};
    auto first = cache.Get(rows);
    Expect(first->imgDescriptors.rows == 2 && cache.Get(rows) == first, "feature subsets are deduplicated and reused");
    auto other = cache.Get(std::vector<uint32_t>{2, 3, 4});
    Expect(first->imgDescriptors.rows == 2 && other->imgDescriptors.rows == 3, "eviction preserves active immutable feature subsets");
}

void TestCandidateParser() {
    auto candidates = CoordinateCandidateParser::Parse("-6901,-708,-22", 0.95f);
    Expect(!candidates.empty() && candidates[0].x == -6901 && candidates[0].y == -708 && candidates[0].z == -22,
        "literal coordinate should parse");

    candidates = CoordinateCandidateParser::Parse("-6904-766,-22", 0.95f);
    Expect(!candidates.empty() && candidates[0].x == -6904 && candidates[0].y == -766 && candidates[0].z == -22,
        "internal minus should recover a missing first comma");

    candidates = CoordinateCandidateParser::Parse("-5808,-474-26", 0.95f);
    Expect(!candidates.empty() && candidates[0].x == -5808 && candidates[0].y == -474 && candidates[0].z == -26,
        "x,y-22 form should recover the final comma");

    candidates = CoordinateCandidateParser::Parse("6901,-708,-22", 0.95f, Coordinate(-6900, -700));
    Expect(candidates.size() >= 2 && candidates[0].x == 6901 && candidates[1].x == -6901,
        "sign fallback should follow the literal candidate");

    Expect(CoordinateCandidateParser::Parse("-6901,-708,-22", 0.64f).empty(),
        "scores below 0.65 should be rejected");
    Expect(CoordinateCandidateParser::Parse("TMQ", 0.99f).empty(), "English noise should be rejected");
    Expect(CoordinateCandidateParser::Parse("福", 0.99f).empty(), "Chinese noise should be rejected");
    Expect(CoordinateCandidateParser::Parse("999999999999,-1,-2", 0.99f).empty(),
        "integer overflow should be rejected");
}

void TestRecoveryController() {
    using Clock = CoordinateRecoveryController::Clock;
    const auto start = Clock::now();
    CoordinateRecoveryController controller;
    controller.SetVisible(true, start);
    Expect(controller.State() == CoordinateLockState::Uninitialized,
        "one visible frame should not start recovery");
    controller.SetVisible(true, start);
    Expect(controller.State() == CoordinateLockState::Recovering,
        "two visible frames should start recovery");
    controller.OnRecognitionSuccess(start);
    Expect(controller.State() == CoordinateLockState::Tracking, "validated OCR should enter Tracking");
    controller.OnContinuityFailure(start);
    Expect(controller.State() == CoordinateLockState::Suspect, "first failure should enter Suspect");
    Expect(controller.CanUseTrustedPosition(start),
        "the first local-tracking failure must retain the trusted marker");
    controller.OnContinuityFailure(start);
    Expect(controller.State() == CoordinateLockState::Suspect, "second failure should remain Suspect");
    controller.OnContinuityFailure(start);
    Expect(controller.State() == CoordinateLockState::Recovering, "third failure should enter Recovering");
    Expect(controller.CanUseTrustedPosition(start + std::chrono::milliseconds(2999)),
        "trusted position should be retained for the three second stale-marker grace period");
    Expect(controller.ShouldHideMarkers(start + std::chrono::milliseconds(3001)),
        "markers should hide after the three second stale-marker grace period expires");
    controller.OnRecognitionFailure();
    controller.OnRecognitionFailure();
    controller.OnRecognitionFailure();
    Expect(controller.ShouldSearchAllScenes(), "three failed OCR batches should enable cross-scene search");
    controller.SetVisible(false, start);
    Expect(controller.State() == CoordinateLockState::Hidden, "hidden HUD should enter Hidden");

    controller.SetVisible(true, start);
    controller.SetVisible(true, start);
    controller.OnRecognitionSuccess(start);
    controller.RestartRecovery();
    Expect(controller.State() == CoordinateLockState::Recovering &&
        !controller.CanUseTrustedPosition(start),
        "teleport recovery should discard the old trusted position immediately");

    controller.OnRecognitionSuccess(start);
    controller.StartRecoveryKeepingTrustedPosition();
    Expect(controller.State() == CoordinateLockState::Recovering &&
        controller.CanUseTrustedPosition(start + std::chrono::seconds(2)),
        "visual recovery should retain the trusted position instead of treating loss as a teleport");
}

void TestMinimapResumePolicy() {
    using Clock = MinimapResumePolicy::Clock;
    const auto start = Clock::time_point{} + std::chrono::seconds(1000);
    const MinimapResumeHint trusted{ MinimapResumeSource::TrustedMinimap, { 1, 6479.59, 3256.37 },
        start - std::chrono::seconds(80) };
    const MinimapResumeHint viewport{ MinimapResumeSource::MapViewport, { 1, 12926.75, 8576.61 },
        start - std::chrono::seconds(1), 7, 9 };
    MinimapResumePolicy policy;
    policy.Begin(38, trusted, viewport, 7, 9, start);
    Expect(policy.Size() == 2, "both old player lock and recent viewport are available during cross-area recovery");
    for (std::uint64_t frame = 1; frame <= 2; ++frame) {
        const auto attempt = policy.NextAttempt(38, frame, start);
        Expect(attempt.has_value() && attempt->hint.source == MinimapResumeSource::TrustedMinimap,
            "the old player lock receives a bounded nearby geometric retry");
        if (!attempt.has_value()) return;
        Expect(attempt->hint.observedAt == trusted.observedAt, "reacquisition cannot renew the trusted confirmation timestamp");
        Expect(!policy.Observe(*attempt, false, {}, start), "failed old-region geometry cannot publish a player position");
        Expect(!policy.NextAttempt(38, frame, start).has_value(), "repeated capture cannot consume another hint attempt");
    }
    auto attempt = policy.NextAttempt(38, 3, start);
    Expect(attempt.has_value() && attempt->hint.source == MinimapResumeSource::MapViewport,
        "two old-region failures advance to the independently verified recent viewport");
    if (!attempt.has_value()) return;
    Expect(!policy.Observe(*attempt, true, { 1, 12924.79, 8574.20 }, start) && policy.AwaitingConfirmation(),
        "even Strong nearby geometry from a viewport hint must wait for another captured minimap");
    Expect(!policy.Observe(*attempt, true, { 1, 12924.79, 8574.20 }, start),
        "the same hint result cannot count twice toward confirmation");
    attempt = policy.NextAttempt(38, 4, start + std::chrono::milliseconds(80));
    Expect(attempt.has_value() && policy.Observe(*attempt, true, { 1, 12926.0, 8576.0 },
        start + std::chrono::milliseconds(80)), "another geometrically supported minimap confirms the new area");

    policy.Begin(39, std::nullopt, viewport, 7, 9, start + std::chrono::seconds(30));
    Expect(policy.Size() == 0, "a viewport older than thirty seconds cannot seed recovery");
    policy.Begin(39, std::nullopt, viewport, 8, 9, start);
    Expect(policy.Size() == 0, "a previous full-map session cannot supply a viewport hint");
    policy.Begin(39, std::nullopt, viewport, 7, 10, start);
    Expect(policy.Size() == 0, "panning or zooming invalidates an uncorrected old viewport center");
    policy.Begin(39, trusted, std::nullopt, 7, 9, start + std::chrono::seconds(221));
    Expect(policy.Size() == 0, "repeated map openings do not keep an expired player location alive");

    policy.Begin(40, std::nullopt, viewport, 7, 9, start);
    const auto oldAttempt = policy.NextAttempt(40, 5, start);
    Expect(oldAttempt.has_value(), "a fresh viewport remains available after discarded stale hints");
    if (!oldAttempt.has_value()) return;
    policy.Observe(*oldAttempt, true, { 1, 12924, 8574 }, start);
    policy.Begin(41, std::nullopt, viewport, 7, 9, start);
    Expect(!policy.AwaitingConfirmation() && !policy.Observe(*oldAttempt, true, { 1, 12924, 8574 }, start) &&
        !policy.NextAttempt(40, 6, start).has_value(), "a UI generation change rejects pending observations and old attempts");

    for (std::uint64_t frame = 6; frame <= 9; ++frame) {
        attempt = policy.NextAttempt(41, frame, start);
        Expect(attempt.has_value(), "an unstable region receives only its finite attempt allowance");
        if (!attempt.has_value()) return;
        Expect(!policy.Observe(*attempt, true, { 1, 12924 + 30.0 * frame, 8574 }, start),
            "disagreeing geometric candidates do not confirm each other");
    }
    Expect(!policy.NextAttempt(41, 10, start).has_value() && !policy.AwaitingConfirmation(),
        "four inconsistent observations exhaust a hint instead of starving global retrieval");

    policy.Begin(42, std::nullopt, viewport, 7, 9, start);
    attempt = policy.NextAttempt(42, 11, start);
    if (!attempt.has_value()) { Expect(false, "fresh viewport exists for expiration test"); return; }
    Expect(!policy.Observe(*attempt, true, { 1, 12924, 8574 }, start + std::chrono::seconds(30)) &&
        !policy.AwaitingConfirmation(), "a hint expiring during verification cannot become a pending player lock");

    MinimapVisualConfirmation confirmation;
    Expect(!confirmation.Observe(1, 1, { 1, 100, 100 }, start), "first global recovery geometry remains pending");
    Expect(!confirmation.Observe(1, 1, { 1, 100, 100 }, start), "a duplicate captured image is not a second visual observation");
    Expect(!confirmation.Observe(1, 2, { 1, 100, 100 }, start + std::chrono::seconds(3)),
        "old pending geometry cannot confirm a later recovery attempt outside its time window");
    Expect(!confirmation.Observe(2, 3, { 1, 100, 100 }, start + std::chrono::seconds(3)),
        "matching geometry from a different UI generation starts a new confirmation");
    confirmation.Reset();
    Expect(!confirmation.Observe(2, 4, { 1, 100, 100 }, start + std::chrono::seconds(3)),
        "failed geometry clears earlier pending agreement");
    Expect(confirmation.Observe(2, 5, { 1, 105, 102 }, start + std::chrono::milliseconds(3080)),
        "two fresh supported observations retain the existing twelve-pixel publication tolerance");
}

void TestMapUiStateController() {
    MapUiStateController controller;
    auto update = controller.Update({ false, true });
    Expect(update.current == MapUiState::Unknown,
        "one gameplay frame should not leave the unknown startup state");
    update = controller.Update({ false, true });
    Expect(update.current == MapUiState::Gameplay,
        "two gameplay frames should confirm gameplay");

    // A compass-colour candidate is deliberately represented as an
    // unconfirmed evidence frame. It must not clear gameplay markers.
    update = controller.Update({ false, true });
    Expect(update.current == MapUiState::Gameplay,
        "unconfirmed big-map candidate must retain gameplay state");
    update = controller.Update({ true, false });
    Expect(update.current == MapUiState::Gameplay,
        "one structurally-confirmed frame should not open the map");
    update = controller.Update({ true, false });
    Expect(update.current == MapUiState::BigMap,
        "two structurally-confirmed frames should confirm the big map");

    // Closing through Escape or a click does not have to produce M. Gameplay
    // must be observed twice before caches are handed back to minimap logic.
    update = controller.Update({ false, true });
    Expect(update.current == MapUiState::BigMap,
        "one gameplay frame should not close a confirmed big map");
    update = controller.Update({ false, true });
    Expect(update.current == MapUiState::Gameplay,
        "two gameplay frames should confirm a closed map");

    for (int frame = 0; frame < 9; ++frame) update = controller.Update({ false, false });
    Expect(update.current == MapUiState::Gameplay,
        "brief missing minimap evidence must retain gameplay marker caches");
    update = controller.Update({ false, false });
    Expect(update.current == MapUiState::Unknown,
        "only sustained missing minimap evidence should clear marker caches");
}

void TestMapCompassVisualDetector() {
#ifndef IMAO_SOURCE_DIR
    Expect(false, "IMAO_SOURCE_DIR must be defined for UI image regression tests");
    return;
#else
    const auto sourceRoot = std::filesystem::path(IMAO_SOURCE_DIR);
    const cv::Mat map = cv::imread((sourceRoot / "Tests" / "MapUi" / "black-shores-map.png").string());
    const cv::Mat gameplay = cv::imread((sourceRoot / "Tests" / "MapUi" / "black-shores-gameplay.png").string());
    Expect(!map.empty() && !gameplay.empty(), "Black Shores UI fixtures should load");
    if (map.empty() || gameplay.empty()) return;
    const RECT rect{ 0, 0, map.cols, map.rows };
    const auto mapDetection = MapUiVisualDetector::DetectBigMapCompass(map, rect);
    const auto gameplayDetection = MapUiVisualDetector::DetectBigMapCompass(gameplay, rect);
    Expect(mapDetection.visible && mapDetection.goldPixels > gameplayDetection.goldPixels,
        "Black Shores big-map compass should be detected");
    Expect(!gameplayDetection.visible,
        "normal Black Shores gameplay should not be classified as big map");
    for (const cv::Size size : {cv::Size(1280, 720), cv::Size(1600, 900),
            cv::Size(1920, 1080), cv::Size(2560, 1440)}) {
        cv::Mat scaledMap, scaledGameplay;
        cv::resize(map, scaledMap, size);
        cv::resize(gameplay, scaledGameplay, size);
        const RECT scaledRect{0, 0, size.width, size.height};
        const bool confirmed = MapUiVisualDetector::DetectBigMapCompass(scaledMap, scaledRect).visible &&
            MapUiVisualDetector::DetectBigMapControls(scaledMap, scaledRect);
        Expect(confirmed, "map controls should confirm a cold start at width " + std::to_string(size.width));
        Expect(!MapUiVisualDetector::DetectBigMapControls(scaledGameplay, scaledRect),
            "gameplay must not contain the pair of map zoom controls");
        MapUiStateController coldStart;
        coldStart.Update({confirmed, false});
        Expect(coldStart.Update({confirmed, false}).current == MapUiState::BigMap,
            "map UI should open without a previous player position or keypress");
        cv::Mat bgra;
        cv::cvtColor(scaledMap, bgra, cv::COLOR_BGR2BGRA);
        Expect(MapUiVisualDetector::DetectBigMapControls(bgra, scaledRect),
            "four-channel capture should preserve map-control evidence");
        scaledMap(cv::Rect(size.width * 92 / 100, 0, size.width - size.width * 92 / 100, size.height)).setTo(0);
        Expect(!MapUiVisualDetector::DetectBigMapControls(scaledMap, scaledRect),
            "compass and terrain alone must not confirm map UI");
    }
    Expect(!MapUiVisualDetector::DetectBigMapControls({}, rect), "empty frame should be rejected");
    Expect(!MapUiVisualDetector::DetectBigMapControls(map, RECT{0, 0, 1, 1}),
        "stale capture geometry should be rejected");
#endif
}

void TestMapViewportPredictor() {
    MapViewportPredictor predictor;
    const int worldScene = Scene::SceneNameToId("World");
    const std::vector<cv::Point2f> corners = {
        { 900.0f, 1900.0f }, { 1100.0f, 1900.0f },
        { 1100.0f, 2100.0f }, { 900.0f, 2100.0f }
    };
    predictor.Confirm(worldScene, Coordinate(1000.0, 2000.0), corners);
    MapViewportPrediction prediction;
    Expect(predictor.GetPrediction(prediction) && prediction.isConfirmed && prediction.sceneId == worldScene,
        "a visual map confirmation should initialize the predictor");
    const auto confirmedRevision = prediction.revision;
    Expect(predictor.SetDragCenter(Coordinate(1025.0, 2015.0)) && predictor.GetPrediction(prediction) &&
        std::abs(prediction.centerMapCoordinate.x - 1025.0) < 0.01 && prediction.revision > confirmedRevision,
        "a drag should update the predicted map center and invalidate old viewport work");
    const auto draggedRevision = prediction.revision;

    cv::Mat source = cv::Mat::zeros(256, 256, CV_8UC1);
    cv::RNG random(17);
    for (int index = 0; index < 72; ++index) {
        const cv::Point point(random.uniform(18, 238), random.uniform(18, 238));
        cv::circle(source, point, random.uniform(3, 10), cv::Scalar(random.uniform(100, 256)), cv::FILLED);
    }
    MapViewportPrediction observed;
    Expect(!predictor.ObserveFrame(source, observed),
        "the first map frame should only establish a lightweight tracking reference");
    cv::Mat shifted;
    const cv::Mat translation = (cv::Mat_<double>(2, 3) << 1.0, 0.0, -8.0, 0.0, 1.0, 0.0);
    cv::warpAffine(source, shifted, translation, source.size(), cv::INTER_LINEAR, cv::BORDER_CONSTANT);
    Expect(predictor.ObserveFrame(shifted, observed) && observed.isPredicted &&
        observed.centerMapCoordinate.x > 1025.0 && observed.revision > draggedRevision,
        "a shifted map crop should move the predicted center and invalidate old viewport work");

    cv::Mat blank = cv::Mat::zeros(256, 256, CV_8UC1);
    MapViewportPrediction held;
    predictor.ObserveFrame(blank, held);
    Expect(predictor.GetPrediction(held) && held.confidence >= 2,
        "one failed frame-to-frame match should not hide a confirmed map marker");
}

void TestWorldSearchPrior() {
#ifndef IMAO_SOURCE_DIR
    Expect(false, "IMAO_SOURCE_DIR must be defined for World prior tests");
    return;
#else
    RuntimeFeatureResources resources;
    resources.visualIndexReady = true;
    resources.baseVisualTileCount = 1;
    resources.visualIndex.tiles = {
        { Scene::SceneNameToId("World"), 0, 0, 0.0f, 0.0f, 384.0f, 384.0f },
        { Scene::SceneNameToId("World"), 1, 0, 384.0f, 0.0f, 768.0f, 384.0f },
        { Scene::SceneNameToId("Tethys"), 0, 0, 0.0f, 0.0f, 384.0f, 384.0f }
    };
    WorldSearchPriorIndex index;
    std::string error;
    const auto sourceRoot = std::filesystem::path(IMAO_SOURCE_DIR);
    Expect(index.Load(sourceRoot / "Assets" / "KuroMap" / "country.json", error),
        "World country hierarchy should load: " + error);
    const auto prior = index.Build(resources, Coordinate(300.0, 180.0), 160.0);
    Expect(prior.valid && prior.sceneId == Scene::SceneNameToId("World") &&
        prior.candidateTileCount == 2,
        "World prior should include only nearby World visual tiles");
    Expect(!prior.areaName.empty(), "World prior should expose a diagnostic area name when hierarchy data exists");
#endif
}

void TestNewSceneRegistry() {
    struct ExpectedScene { int id; const char* name; int state; };
    const std::array<ExpectedScene, 3> expected = {{
        { 6, "LowerVault", 902 },
        { 7, "Darkplain", 909 },
        { 8, "TimeRiftRuins", 910 }
    }};
    for (const auto& entry : expected) {
        const auto* scene = Scene::Find(entry.id);
        Expect(scene != nullptr && std::string(scene->name) == entry.name && scene->kuroStateId == entry.state &&
            scene->requiresGameValidation && scene->scale > 0.0,
            std::string("new scene registry is invalid: ") + entry.name);
        Expect(Scene::SceneNameToId(entry.name) == entry.id,
            std::string("new scene name lookup failed: ") + entry.name);
    }
}

void TestFeatureBinaryCodec() {
    const auto testDirectory = std::filesystem::temp_directory_path() /
        ("imao-feature-codec-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(testDirectory);
    const auto cleanup = wil::scope_exit([&] { std::filesystem::remove_all(testDirectory); });

    ImageFeatureData input;
    input.imgKeypoints = {
        cv::KeyPoint(1.25f, 2.5f, 3.75f, 4.0f, 5.0f, 6, 7),
        cv::KeyPoint(-8.0f, 9.5f, 10.0f, 11.0f, 12.0f, -13, 14),
        cv::KeyPoint(15.0f, -16.0f, 17.0f, 18.0f, 19.0f, 20, -21)
    };
    input.imgDescriptors.create(3, 128, CV_32FC1);
    for (int row = 0; row < input.imgDescriptors.rows; ++row) {
        for (int column = 0; column < input.imgDescriptors.cols; ++column) {
            input.imgDescriptors.at<float>(row, column) = static_cast<float>(row * 128 + column) / 17.0f;
        }
    }

    std::array<std::uint8_t, 32> sourceHash{};
    for (std::size_t index = 0; index < sourceHash.size(); ++index) sourceHash[index] = static_cast<std::uint8_t>(index);
    const auto first = testDirectory / "first.imf";
    const auto second = testDirectory / "second.imf";
    std::string error;
    Expect(FeatureBinaryCodec::Save(first, input, sourceHash, error), "fixture IMF should save: " + error);
    Expect(FeatureBinaryCodec::Save(second, input, sourceHash, error), "second fixture IMF should save: " + error);
    Expect(ReadAll(first) == ReadAll(second), "same input should produce deterministic IMF bytes");

    ImageFeatureData output;
    Expect(FeatureBinaryCodec::Load(first, output, error), "fixture IMF should load: " + error);
    Expect(output.imgKeypoints.size() == input.imgKeypoints.size() &&
        cv::countNonZero(output.imgDescriptors != input.imgDescriptors) == 0,
        "fixture round trip should preserve descriptors");

    auto corrupted = ReadAll(first);
    corrupted.back() ^= 0xff;
    const auto corruptedPath = testDirectory / "corrupted.imf";
    WriteAll(corruptedPath, corrupted);
    Expect(!FeatureBinaryCodec::Load(corruptedPath, output, error) && error.find("SHA-256") != std::string::npos,
        "payload corruption should fail its hash");

    corrupted = ReadAll(first);
    corrupted.resize(corrupted.size() - 1);
    const auto truncatedPath = testDirectory / "truncated.imf";
    WriteAll(truncatedPath, corrupted);
    Expect(!FeatureBinaryCodec::Load(truncatedPath, output, error), "truncated IMF should fail safely");

    corrupted = ReadAll(first);
    corrupted[8] = 2;
    const auto versionPath = testDirectory / "version.imf";
    WriteAll(versionPath, corrupted);
    Expect(!FeatureBinaryCodec::Load(versionPath, output, error) && error.find("version") != std::string::npos,
        "unsupported IMF version should fail safely");

    corrupted = ReadAll(first);
    WriteLittleEndian32(corrupted, 20, 0xffffffffu);
    const auto oversizedPath = testDirectory / "oversized.imf";
    WriteAll(oversizedPath, corrupted);
    Expect(!FeatureBinaryCodec::Load(oversizedPath, output, error),
        "oversized keypoint count should fail before allocation");

    corrupted = ReadAll(first);
    WriteLittleEndian32(corrupted, 28, 127);
    const auto shapePath = testDirectory / "shape.imf";
    WriteAll(shapePath, corrupted);
    Expect(!FeatureBinaryCodec::Load(shapePath, output, error),
        "non-128 descriptor width should fail safely");
}

MapVisualIndex CreateVisualIndexFixture(const std::array<std::uint8_t, 32>& sourceHash) {
    MapVisualIndex index;
    index.featureCount = 1;
    index.sourceImfSha256 = sourceHash;
    index.vocabulary = cv::Mat::zeros(
        MapVisualIndex::WordCount, MapVisualIndex::DescriptorColumns, CV_32FC1);
    index.vocabulary.at<float>(0, 0) = 1.0f;
    index.tiles.push_back({ 1, 0, 0, 0.0f, 0.0f, 384.0f, 384.0f, 0, 1, 0, 1, 1.0f });
    index.histograms.push_back({ 0, 1.0f });
    index.featureRows.push_back(0);
    index.postings.push_back({ 0, 0, 1.0f });
    return index;
}

void TestMapVisualIndexCodec() {
    const auto testDirectory = std::filesystem::temp_directory_path() /
        ("imao-visual-index-codec-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(testDirectory);
    const auto cleanup = wil::scope_exit([&] { std::filesystem::remove_all(testDirectory); });

    std::array<std::uint8_t, 32> sourceHash{};
    for (std::size_t index = 0; index < sourceHash.size(); ++index) {
        sourceHash[index] = static_cast<std::uint8_t>(31 - index);
    }
    auto fixture = CreateVisualIndexFixture(sourceHash);
    const auto first = testDirectory / "first.imx";
    const auto second = testDirectory / "second.imx";
    std::string error;
    Expect(MapVisualIndexCodec::Save(first, fixture, error), "fixture IMX should save: " + error);
    Expect(MapVisualIndexCodec::Save(second, fixture, error), "second fixture IMX should save: " + error);
    Expect(ReadAll(first) == ReadAll(second), "same input should produce deterministic IMX bytes");

    MapVisualIndex output;
    Expect(MapVisualIndexCodec::Load(first, sourceHash, 1, output, error),
        "fixture IMX should load: " + error);
    Expect(output.tiles.size() == 1 && output.featureRows == std::vector<std::uint32_t>{ 0 } &&
        output.postingOffsets.size() == MapVisualIndex::WordCount + 1,
        "fixture IMX round trip should preserve its tile, row, and inverted index");

    auto wrongSourceHash = sourceHash;
    wrongSourceHash[0] ^= 0xff;
    Expect(!MapVisualIndexCodec::Load(first, wrongSourceHash, 1, output, error),
        "IMX should reject a different source IMF hash");

    auto corrupted = ReadAll(first);
    corrupted.resize(corrupted.size() - 1);
    const auto truncated = testDirectory / "truncated.imx";
    WriteAll(truncated, corrupted);
    Expect(!MapVisualIndexCodec::Load(truncated, sourceHash, 1, output, error),
        "truncated IMX should fail safely");

    corrupted = ReadAll(first);
    corrupted[8] = 2;
    const auto version = testDirectory / "version.imx";
    WriteAll(version, corrupted);
    Expect(!MapVisualIndexCodec::Load(version, sourceHash, 1, output, error),
        "unsupported IMX version should fail safely");

    corrupted = ReadAll(first);
    corrupted[128] ^= 0xff;
    const auto vocabularyHash = testDirectory / "vocabulary-hash.imx";
    WriteAll(vocabularyHash, corrupted);
    Expect(!MapVisualIndexCodec::Load(vocabularyHash, sourceHash, 1, output, error) &&
        error.find("vocabulary SHA-256") != std::string::npos,
        "IMX should verify the vocabulary hash independently");

    auto badRowFixture = fixture;
    badRowFixture.featureRows[0] = 1;
    const auto badRow = testDirectory / "bad-row.imx";
    Expect(MapVisualIndexCodec::Save(badRow, badRowFixture, error), "invalid-row fixture should save");
    Expect(!MapVisualIndexCodec::Load(badRow, sourceHash, 1, output, error) &&
        error.find("out of range") != std::string::npos,
        "IMX should reject an out-of-range feature row after a valid payload hash");

    auto badPostingFixture = fixture;
    badPostingFixture.postings[0].tileIndex = 1;
    const auto badPosting = testDirectory / "bad-posting.imx";
    Expect(MapVisualIndexCodec::Save(badPosting, badPostingFixture, error),
        "invalid-posting fixture should save");
    Expect(!MapVisualIndexCodec::Load(badPosting, sourceHash, 1, output, error) &&
        error.find("posting") != std::string::npos,
        "IMX should reject an inverted-list posting outside the tile table");
}

void TestManifestShardLineEndings() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("imao-manifest-shard-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    const auto cleanup = wil::scope_exit([&] { std::filesystem::remove_all(directory); });
    const auto manifest = directory / "manifest.json";
    const auto indexPath = directory / "visual-index.imx";
    const std::string lf = "{\n  \"scene\": \"World\"\n}\n";
    const std::string crlf = "{\r\n  \"scene\": \"World\"\r\n}\r\n";
    WriteAll(manifest, {lf.begin(), lf.end()});
    std::string error;
    std::array<std::uint8_t, 32> hash{};
    Expect(FeatureBinaryCodec::Sha256File(manifest, hash, error), "manifest should hash");
    auto fixture = CreateVisualIndexFixture(hash);
    Expect(MapVisualIndexCodec::Save(indexPath, fixture, error), "manifest shard should save");
    MapVisualIndex output;
    Expect(MapVisualIndexCodec::LoadManifestShard(indexPath, manifest, 1, output, error),
        "LF source should load its shard: " + error);
    WriteAll(manifest, {crlf.begin(), crlf.end()});
    Expect(MapVisualIndexCodec::LoadManifestShard(indexPath, manifest, 1, output, error),
        "CRLF checkout should load the original LF shard: " + error);
    Expect(!MapVisualIndexCodec::LoadManifestShard(indexPath, manifest, 2, output, error) &&
        error.find("feature count") != std::string::npos,
        "newline compatibility must preserve feature count validation");
    auto damaged = ReadAll(indexPath);
    damaged.back() ^= 0xff;
    const auto damagedPath = directory / "damaged.imx";
    WriteAll(damagedPath, damaged);
    Expect(!MapVisualIndexCodec::LoadManifestShard(damagedPath, manifest, 1, output, error),
        "newline compatibility must reject a corrupt payload");
    const std::string changed = "{\r\n  \"scene\": \"Changed\"\r\n}\r\n";
    WriteAll(manifest, {changed.begin(), changed.end()});
    Expect(!MapVisualIndexCodec::LoadManifestShard(indexPath, manifest, 1, output, error) &&
        error.find("source SHA-256") != std::string::npos,
        "a changed manifest must still fail source validation");
    WriteAll(manifest, {crlf.begin(), crlf.end()});
    Expect(FeatureBinaryCodec::Sha256File(manifest, hash, error), "CRLF manifest should hash");
    fixture.sourceImfSha256 = hash;
    Expect(MapVisualIndexCodec::Save(indexPath, fixture, error), "legacy CRLF shard should save");
    Expect(MapVisualIndexCodec::LoadManifestShard(indexPath, manifest, 1, output, error),
        "a shard originally generated from CRLF must remain supported");
#ifdef IMAO_SOURCE_DIR
    const auto pack = std::filesystem::path(IMAO_SOURCE_DIR) / "Assets/FeaturesDatas/DreamzhouCandidate";
    Expect(MapVisualIndexCodec::LoadManifestShard(pack / "visual-index.imx", pack / "manifest.json",
        532, output, error), "shipped candidate index should load: " + error);
#endif
}
}

void TestMapViewportGeometry() {
    std::vector<cv::Point2f> source, target;
    for (int y = 0; y < 5; ++y) {
        for (int x = 0; x < 5; ++x) {
            source.emplace_back(30.0f + x * 80.0f, 40.0f + y * 75.0f);
            target.emplace_back(source.back() * 1.7f + cv::Point2f(-7200.0f, 1400.0f));
        }
    }
    // A few unrelated icons must not move the zoom/pan estimate.
    for (int i = 0; i < 5; ++i) target[i] += cv::Point2f(200.0f, -100.0f);
    cv::Mat mask;
    auto fit = FitMapViewportTransform(source, target, mask);
    Expect(!fit.empty(), "map zoom/pan with outliers should localize");
    if (!fit.empty()) {
        Expect(std::abs(fit.at<double>(0, 0) - 1.7) < 0.001 &&
            std::abs(fit.at<double>(0, 2) + 7200.0) < 0.01 &&
            std::abs(fit.at<double>(1, 2) - 1400.0) < 0.01,
            "map fit should recover the true zoom and translation");
        Expect(cv::countNonZero(mask) == 20, "unrelated matches should be rejected");
    }
    // This projective mapping has zero homography residual, but is impossible
    // for the north-up map and used to pass the viewport geometry checks.
    const cv::Mat projective = (cv::Mat_<double>(3, 3) <<
        1.7, 0.2, -7200, 0.1, 1.7, 1400, 0.001, 0.002, 1);
    cv::perspectiveTransform(source, target, projective);
    fit = FitMapViewportTransform(source, target, mask);
    Expect(fit.empty() || cv::countNonZero(mask) < 12,
        "perspective false matches must not be accepted as a game map");
    for (std::size_t i = 0; i < source.size(); ++i)
        target[i] = cv::Point2f(-source[i].y, source[i].x);
    Expect(FitMapViewportTransform(source, target, mask).empty(),
        "a rotated false match must be rejected");
    cv::Mat supportMask = cv::Mat::ones(static_cast<int>(source.size()), 1, CV_8UC1);
    Expect(HasMapViewportSupport(source, supportMask, cv::Size(560, 560)),
        "distributed terrain should have enough spatial support");
    auto clustered = source;
    for (auto& point : clustered) point *= 0.01f;
    Expect(!HasMapViewportSupport(clustered, supportMask, cv::Size(560, 560)),
        "a tiny feature cluster must not establish map zoom");
    for (std::size_t i = 0; i < clustered.size(); ++i)
        clustered[i] = cv::Point2f(10.0f * i, 10.0f * i);
    Expect(!HasMapViewportSupport(clustered, supportMask, cv::Size(560, 560)),
        "collinear terrain must not establish map geometry");
    source.resize(4); target.resize(4);
    Expect(FitMapViewportTransform(source, target, mask).empty(),
        "insufficient matches must not produce a viewport");
}

void TestRecordedMapViewportGeometry() {
#ifdef IMAO_SOURCE_DIR
    // SURF correspondences from the 2026-09-06 diagnostic map frame, using
    // six nearby search centers. The old projective fit moved hundreds of
    // map units even though the screenshot never changed.
    std::ifstream input(std::filesystem::path(IMAO_SOURCE_DIR) /
        "Tests/MapUi/viewport-correspondences.txt");
    Expect(input.good(), "recorded viewport correspondences should be available");
    int count = 0, cases = 0, accepted = 0;
    while (input >> count) {
        std::vector<cv::Point2f> source(count), target(count);
        for (int i = 0; i < count; ++i)
            input >> source[i].x >> source[i].y >> target[i].x >> target[i].y;
        Expect(input.good(), "recorded viewport correspondences should be complete");
        if (!input) break;
        ++cases;
        cv::Mat mask;
        const auto fit = FitMapViewportTransform(source, target, mask);
        if (fit.empty()) continue; // Rejecting ambiguous support is safe.
        ++accepted;
        const cv::Point2d center(fit.at<double>(0, 2) + 280 * fit.at<double>(0, 0),
            fit.at<double>(1, 2) + 280 * fit.at<double>(1, 1));
        Expect(cv::norm(center - cv::Point2d(-6807.6, 1761.3)) < 2.0,
            "the same map frame must retain its center across search scopes");
        Expect(std::abs(fit.at<double>(0, 0) - 0.558) < 0.01 &&
            cv::countNonZero(mask) >= 12,
            "recorded map zoom must retain enough terrain support");
        Expect(HasMapViewportSupport(source, mask, cv::Size(560, 560)) &&
            static_cast<double>(cv::countNonZero(mask)) / count >= 0.35,
            "recorded map must pass the runtime spatial support and inlier ratio gates");
    }
    Expect(cases == 6 && accepted >= 4, "recorded map should localize across nearby searches");
#endif
}

void TestRuntimeDataSafety() {
    const auto compact = KuroPositionToGameCoordinates(3200, 1700);
    Expect(compact.x == 32 && compact.y == 17, "Kuro compact values must retain upstream centi-units");
    const auto origin = KuroPositionToGameCoordinates(0, 0);
    Expect(origin.x == 0 && origin.y == 0, "upstream origin must remain a representable position");
    const auto before = KuroPositionToGameCoordinates(-10000, 0);
    const auto after = KuroPositionToGameCoordinates(-10001, 0);
    Expect(std::abs(after.x - before.x + 0.01) < 1e-9, "coordinate scale must remain continuous across the former magnitude threshold");
    bool rejectedNonFinite = false;
    try { KuroPositionToGameCoordinates(std::numeric_limits<double>::infinity(), 0); }
    catch (const std::invalid_argument&) { rejectedNonFinite = true; }
    Expect(rejectedNonFinite, "non-finite game coordinates must be rejected");
    struct Group { std::string nameId; std::vector<int> points; };
    SceneItemStore<Group> items;
    for (int scene = 1; scene <= 8; ++scene) {
        items.Add(scene, {"shared", {1, 2}});
        items.Add(scene, {"shared", {1, 2}});
        Expect(items.Read(scene)->size() == 1, "repeated selection must not duplicate any scene");
    }
    const auto inFlight = items.Read(3);
    items.Remove("shared");
    Expect(inFlight->size() == 1 && inFlight->front().points.size() == 2,
        "an in-flight renderer must retain its snapshot during deselection");
    for (int scene = 1; scene <= 8; ++scene)
        Expect(items.Read(scene)->empty(), "deselection must include Fabricatorium and all new scenes");
    std::atomic_bool invalid = false;
    std::jthread reader([&] {
        for (int i = 0; i < 2000; ++i) {
            auto snapshot = items.Read(3);
            if (snapshot->size() > 1 || (!snapshot->empty() && snapshot->front().points.size() != 2)) invalid = true;
        }
    });
    for (int i = 0; i < 1000; ++i) { items.Add(3, {"shared", {1, 2}}); items.Remove("shared"); }
    reader.join();
    Expect(!invalid, "selection changes must publish complete snapshots");
    for (const auto* name : {"../other", "..\\other", "C:other", "CON", "nul.json", "LPT1", "route.", ""}) {
        bool rejected = false;
        try { ValidateUserFileName(name); } catch (const std::invalid_argument&) { rejected = true; }
        Expect(rejected, "unsafe route names must be rejected before IO");
    }
    ValidateUserFileName("路线 01");
    const auto folder = std::filesystem::temp_directory_path() / ("imao-atomic-test-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(folder);
    const auto path = folder / "points.json";
    WriteTextAtomically(path, "old");
    HANDLE held = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Expect(held != INVALID_HANDLE_VALUE, "failure injection must lock the destination");
    bool failed = false;
    try { WriteTextAtomically(path, "replacement"); } catch (const std::exception&) { failed = true; }
    if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
    std::ifstream input(path);
    std::string retained;
    input >> retained;
    input.close();
    Expect(failed && retained == "old", "failed commit must preserve the previous user file");
    WriteTextAtomically(path, "new");
    std::ifstream committed(path);
    committed >> retained;
    committed.close();
    Expect(retained == "new", "successful commit must replace the entire file");
    std::filesystem::remove_all(folder);
}

void TestFramePublication() {
    SnapshotChannel<OverlayFrame> channel;
    Expect(!channel.Read()->Fresh(), "empty frame must not be rendered");
    OverlayFrame frame;
    frame.frameId = 1; frame.playerScene = 1;
    frame.capturedAt = std::chrono::steady_clock::now();
    frame.minimapMarkers.markers.push_back({"point", "type", {}, {}, false, {8, 1, "floor", "level"}});
    channel.Publish(frame);
    const auto retained = channel.Read();
    std::atomic_bool torn{false};
    std::thread writer([&] {
        for (uint64_t i = 2; i < 10000; ++i) {
            OverlayFrame next; next.frameId = i; next.playerScene = static_cast<int>(i);
            channel.Publish(std::move(next));
        }
    });
    for (int i = 0; i < 10000; ++i) {
        auto value = channel.Read();
        if (value->frameId != static_cast<uint64_t>(value->playerScene)) torn = true;
    }
    writer.join();
    Expect(!torn, "concurrent frame readers must never mix generations");
    Expect(retained->frameId == 1 && retained->minimapMarkers.markers[0].layer.floorId == "floor",
        "published frames retain point identities and survive replacement");
    frame.capturedAt -= std::chrono::seconds(1);
    Expect(!frame.Fresh(), "stalled capture must expire even when a frame is present");
    frame.maximumAge = std::chrono::milliseconds(2000);
    Expect(frame.Fresh(), "an explicitly slow update cadence must not expire between scheduled captures");
    frame.capturedAt -= std::chrono::seconds(2);
    Expect(!frame.Fresh(), "slow update cadence still rejects a stalled capture");
}

int main() {
    TestFramePublication();
    TestRuntimeDataSafety();
    {
        UniqueMapFeatures unique;
        cv::KeyPoint first(10.0f, 20.0f, 9.0f);
        cv::Mat descriptor = cv::Mat::ones(1, 128, CV_32F);
        Expect(unique.Insert(first, descriptor), "first map observation must survive");
        Expect(!unique.Insert(first, descriptor.clone()), "overlapping pack copies must collapse");
        auto distant = first;
        distant.pt.x += 500.0f;
        Expect(unique.Insert(distant, descriptor), "identical appearance at another location must remain ambiguous");
        auto changed = descriptor.clone();
        changed.at<float>(0, 0) = 0.5f;
        Expect(unique.Insert(first, changed), "different observations at the same position must survive");
        auto original = descriptor.clone();
        descriptor.at<float>(0, 0) = 0.75f;
        Expect(!unique.Insert(first, original), "deduplication owns its bytes even when the caller changes a source matrix");
        Expect(unique.Insert(first, descriptor), "changed descriptors at existing coordinates remain distinct");
    }
    Expect(ShouldRetryWithoutTemporalMask(130, 15) && ShouldRetryWithoutTemporalMask(90, 6),
        "recorded moving minimaps must recover their static terrain features");
    Expect(!ShouldRetryWithoutTemporalMask(100, 70) && !ShouldRetryWithoutTemporalMask(0, 0),
        "healthy masks and empty terrain must not trigger fallback");
    Expect(!HasReacquisitionSupport(false, 3, 0.157895, 2),
        "recorded three-vote distant false lock must not seed a viewport");
    Expect(HasReacquisitionSupport(true, 9, 0.75, 2),
        "distributed geometric recovery must remain available");
    TestMapViewportGeometry();
    TestRecordedMapViewportGeometry();
    TestExactDescriptorMatching();
    TestCandidateParser();
    TestRecoveryController();
    TestMinimapResumePolicy();
    TestCoarseSearchCoverage(Expect);
    TestOverlayVisibility(Expect);
    TestGamepadContext(Expect);
    TestGamepadCursorTargets(Expect);
    TestGamepadWorldActions(Expect);
    TestRouteGamepad(Expect);
    TestMapTools(Expect);
    TestOverlayPanelLayout(Expect);
    TestControllerMapUi(Expect);
    TestScreenMotionTracker(Expect);
    TestMinimapTrackingGeometry(Expect);
    TestImageAnchoredOverlay(Expect);
    TestMinimapHudEvidence(Expect);
    TestFineViewportMotion(Expect);
	TestMapUiStateController();
	TestMapCompassVisualDetector();
	TestMapViewportPredictor();
	TestWorldSearchPrior();
    TestNewSceneRegistry();
    TestFeatureBinaryCodec();
    TestMapVisualIndexCodec();
    TestManifestShardLineEndings();
    if (failures != 0) {
        std::cerr << failures << " optimization test(s) failed.\n";
        return 1;
    }
    std::cout << "All optimization tests passed.\n";
    return 0;
}
