#include "Coordinate/IdentifyWorldCoordinates/CoordinateCandidate.h"
#include "Coordinate/IdentifyWorldCoordinates/CoordinateRecoveryController.h"
#include "App/MapUiStateController.h"
#include "App/MapUiVisualDetector.h"
#include "App/MapViewportPredictor.h"
#include "App/WorldSearchPrior.h"
#include "Feature/Processing/FeatureBinaryCodec.h"
#include "Feature/VisualIndex/MapVisualIndex.h"

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
    Expect(controller.CanUseTrustedPosition(start + std::chrono::seconds(1)),
        "trusted position should be held for less than two seconds");
    Expect(controller.ShouldHideMarkers(start + std::chrono::milliseconds(2001)),
        "markers should hide after the two second hold expires");
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
}

void TestMapUiStateController() {
    MapUiStateController controller;
    auto update = controller.Update({ true, false });
    Expect(update.current == MapUiState::Unknown,
        "one compass frame should not open the map");
    update = controller.Update({ true, false });
    Expect(update.current == MapUiState::BigMap,
        "two compass frames should confirm the big map");

    // Closing through Escape or a click does not have to produce M.  The first
    // gameplay frame must already remove the confirmed map state and caches,
    // while retaining a distinct transition state for the saved player hint.
    update = controller.Update({ false, true });
    Expect(update.current == MapUiState::LeavingBigMap && update.changed,
        "a conflicting gameplay frame should hide big-map markers immediately");
    update = controller.Update({ false, true });
    Expect(update.current == MapUiState::Gameplay,
        "two gameplay frames should confirm a closed map");

    update = controller.Update({ false, false });
    Expect(update.current == MapUiState::Gameplay,
        "one Unknown map-UI frame should retain the last stable UI state");
    update = controller.Update({ false, false });
    Expect(update.current == MapUiState::Gameplay,
        "two Unknown map-UI frames should retain the last stable UI state");
    update = controller.Update({ false, false });
    Expect(update.current == MapUiState::Unknown,
        "three Unknown map-UI frames should clear the stale overlay state");
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
}

int main() {
    TestCandidateParser();
    TestRecoveryController();
	TestMapUiStateController();
	TestMapCompassVisualDetector();
	TestMapViewportPredictor();
	TestWorldSearchPrior();
    TestNewSceneRegistry();
    TestFeatureBinaryCodec();
    TestMapVisualIndexCodec();
    if (failures != 0) {
        std::cerr << failures << " optimization test(s) failed.\n";
        return 1;
    }
    std::cout << "All optimization tests passed.\n";
    return 0;
}
