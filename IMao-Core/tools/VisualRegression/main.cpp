#include "Coordinate/VisualLocalization/GlobalVisualLocalizer.h"
#include "Coordinate/VisualLocalization/RecoveryPolicy.h"
#include "App/MapViewportLocalizer.h"
#include "App/MinimapResumePolicy.h"
#include "Coordinate/locationCalculator/MapCoordinate.h"
#include "Feature/CandidateFeaturePack.h"
#include "Feature/KuroTileFeaturePack.h"
#include "Feature/Processing/FeatureBinaryCodec.h"
#include "Feature/VisualIndex/MapVisualIndex.h"
#include "ImageProcessing/ImageProcessing.h"

#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <Windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
struct PendingMarginal {
    std::uint64_t frameId = 0;
    VisualLocalizationCandidate candidate;
};

double Distance(const Coordinate& left, const Coordinate& right) {
    return std::hypot(left.x - right.x, left.y - right.y);
}

double Percentile(std::vector<double> values, double percentile) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(std::ceil(percentile * values.size()) - 1.0);
    return values[std::min(index, values.size() - 1)];
}

std::uint64_t WorkingSetBytes() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) return 0;
    return static_cast<std::uint64_t>(counters.WorkingSetSize);
}

bool MergeVisualShard(MapVisualIndex& base, const MapVisualIndex& shard,
    std::uint32_t featureRowBase, std::string& error) {
    if (base.vocabularySha256 != shard.vocabularySha256) {
        error = "visual shard vocabulary mismatch";
        return false;
    }
    const auto tileBase = static_cast<std::uint32_t>(base.tiles.size());
    const auto histogramBase = static_cast<std::uint32_t>(base.histograms.size());
    const auto storedRowBase = static_cast<std::uint32_t>(base.featureRows.size());
    for (const auto& source : shard.tiles) {
        auto tile = source;
        tile.histogramOffset += histogramBase;
        tile.featureRowOffset += storedRowBase;
        base.tiles.push_back(tile);
    }
    base.histograms.insert(base.histograms.end(), shard.histograms.begin(), shard.histograms.end());
    for (const auto row : shard.featureRows) base.featureRows.push_back(row + featureRowBase);
    for (const auto& source : shard.postings) {
        auto posting = source;
        posting.tileIndex += tileBase;
        base.postings.push_back(posting);
    }
    std::sort(base.postings.begin(), base.postings.end(), [](const auto& left, const auto& right) {
        if (left.wordId != right.wordId) return left.wordId < right.wordId;
        return left.tileIndex < right.tileIndex;
    });
    base.featureCount += shard.featureCount;
    return MapVisualIndexCodec::BuildPostingOffsets(base, error);
}

std::shared_ptr<RuntimeFeatureResources> LoadResources(
    const std::filesystem::path& repositoryRoot, std::string& error) {
    const auto featureRoot = repositoryRoot / "Assets" / "FeaturesDatas";
    auto resources = std::make_shared<RuntimeFeatureResources>();
    std::array<std::uint8_t, 32> sourceHash{};
    if (!FeatureBinaryCodec::Load(featureRoot / "Map_features.imf", resources->map,
        error, nullptr, &sourceHash)) return {};
    resources->visualIndexReady = MapVisualIndexCodec::Load(
        featureRoot / "Map_visual_index.imx", sourceHash,
        static_cast<std::uint32_t>(resources->map.imgKeypoints.size()),
        resources->visualIndex, error);
    if (!resources->visualIndexReady) return {};
    resources->baseVisualTileCount = static_cast<std::uint32_t>(resources->visualIndex.tiles.size());

    const auto kuroPacks = KuroTileFeaturePack::LoadRegistered(featureRoot.string());
    for (const auto& kuro : kuroPacks) {
        if (!kuro.loaded) continue;
        const auto rowBase = static_cast<std::uint32_t>(resources->map.imgKeypoints.size());
        const auto firstShardTile = static_cast<std::uint32_t>(resources->visualIndex.tiles.size());
        MapVisualIndex shard;
        if (!MapVisualIndexCodec::Load(
                featureRoot / "KuroTilePacks" / kuro.directoryName / "visual-index.imx", kuro.sourceSha256,
                static_cast<std::uint32_t>(kuro.featureData.imgKeypoints.size()), shard, error) ||
            !MergeVisualShard(resources->visualIndex, shard, rowBase, error)) return {};
        resources->kuroVisualShards.push_back({
            kuro.sceneId, firstShardTile, static_cast<std::uint32_t>(shard.tiles.size()) });
        CandidateFeaturePack::AppendFeatures(resources->map, kuro.featureData);
        CandidateFeaturePack::AppendFeatures(resources->kuroTileFeatures, kuro.featureData);
    }
    const auto candidates = CandidateFeaturePack::LoadRegisteredCandidates(featureRoot.string());
    for (const auto& candidate : candidates) {
        if (!candidate.loaded) continue;
        const auto rowBase = static_cast<std::uint32_t>(resources->map.imgKeypoints.size());
        std::array<std::uint8_t, 32> shardSourceHash{};
        MapVisualIndex shard;
        if (!FeatureBinaryCodec::Sha256File(
                featureRoot / candidate.directoryName / "manifest.json", shardSourceHash, error) ||
            !MapVisualIndexCodec::Load(
                featureRoot / candidate.directoryName / "visual-index.imx", shardSourceHash,
                static_cast<std::uint32_t>(candidate.featureData.imgKeypoints.size()), shard, error) ||
            !MergeVisualShard(resources->visualIndex, shard, rowBase, error)) return {};
        CandidateFeaturePack::AppendFeatures(resources->map, candidate.featureData);
        CandidateFeaturePack::AppendFeatures(resources->curatedCandidates, candidate.featureData);
    }
    return resources;
}

nlohmann::json SerializeCandidate(const VisualLocalizationCandidate& candidate) {
    return {
        {"sceneId", candidate.sceneId}, {"mapX", candidate.mapCenter.x},
        {"mapY", candidate.mapCenter.y}, {"rotationDegrees", candidate.rotationDegrees},
        {"scale", candidate.scale}, {"inliers", candidate.inlierCount},
        {"inlierRatio", candidate.inlierRatio},
        {"medianReprojectionError", candidate.medianReprojectionError},
        {"coveredQuadrants", candidate.coveredQuadrants},
        {"retrievalScore", candidate.retrievalScore},
        {"translationDescriptorScore", candidate.translationDescriptorScore},
        {"ocrHintMatched", candidate.ocrHintMatched},
        {"quality", GlobalVisualLocalizer::QualityName(candidate.quality)}
    };
}

cv::Mat ApplyTransform(const cv::Mat& source, const nlohmann::json& sample) {
    if (!sample.contains("transform") || sample.at("transform").is_null()) return source;
    const auto& transform = sample.at("transform");
    cv::Mat output = source.clone();
    const double resolutionScale = transform.value("resolutionScale", 1.0);
    if (resolutionScale > 0.0 && resolutionScale != 1.0)
        cv::resize(output, output, {}, resolutionScale, resolutionScale, cv::INTER_AREA);
    if (transform.value("blank", false)) {
        output.setTo(cv::Scalar::all(0));
        return output;
    }
    if (transform.value("noise", false)) {
        cv::RNG random(0x494d414f);
        random.fill(output, cv::RNG::UNIFORM, 0, 256);
        return output;
    }
    const double rotation = transform.value("rotationDegrees", 0.0);
    const double scale = transform.value("scale", 1.0);
    if (rotation != 0.0 || scale != 1.0) {
        const cv::Point2f center(output.cols / 2.0f, output.rows / 2.0f);
        const auto matrix = cv::getRotationMatrix2D(center, rotation, scale);
        cv::warpAffine(output, output, matrix, output.size(), cv::INTER_LINEAR, cv::BORDER_REFLECT_101);
    }
    const double alpha = transform.value("brightnessAlpha", 1.0);
    const double beta = transform.value("brightnessBeta", 0.0);
    if (alpha != 1.0 || beta != 0.0) output.convertTo(output, -1, alpha, beta);
    const int blurKernel = transform.value("blurKernel", 0);
    if (blurKernel >= 3 && blurKernel % 2 == 1) {
        cv::GaussianBlur(output, output, cv::Size(blurKernel, blurKernel), 0.0);
    }
    const int centerOcclusionRadius = transform.value("centerOcclusionRadius", 0);
    if (centerOcclusionRadius > 0) {
        cv::circle(output, cv::Point(output.cols / 2, output.rows / 2), centerOcclusionRadius,
            cv::Scalar::all(0), cv::FILLED);
    }
    return output;
}

// This scenario checks recovery behaviour against a previously investigated
// incident. Its coordinates come from logs and matching cross-checks, not an
// independent ground-truth annotation; keep it separate from accuracy metrics.
__declspec(noinline) int ReplayRecoverySequence(const std::filesystem::path& root,
    const nlohmann::json& manifest, const std::filesystem::path& reportPath,
    std::shared_ptr<RuntimeFeatureResources> resources) {
    if (!manifest.contains("provenance") || !manifest.at("provenance").is_object() ||
        manifest.at("provenance").value("independentGroundTruth", true)) {
        std::cerr << "Recovery scenarios require explicit non-independent reference provenance.\n";
        return 2;
    }
    std::string error;
    if (!GlobalVisualLocalizer::Initialize(resources, error)) {
        std::cerr << error << '\n'; return 1;
    }
    nlohmann::json results = nlohmann::json::array();
    int failed = 0, recovered = 0, confirmed = 0, tracked = 0, negatives = 0;
    std::uint64_t frameId = 0, sessionId = 0, captureId = 0;
    std::size_t maximumSearchOffset = 0;
    std::string activeSession;
    MinimapVisualConfirmation confirmation;
    MinimapResumePolicy resumePolicy;
    const bool usesResumeHints = manifest.contains("resumeHints");
    std::optional<VisualLocalizationCandidate> trusted;
    for (const auto& sample : manifest.at("samples")) {
        ++captureId;
        if (sample.contains("worldHint") || sample.contains("expected")) {
            std::cerr << "Recovery scenarios cannot inject world hints or claim independent labels.\n";
            GlobalVisualLocalizer::Shutdown(); return 2;
        }
        const auto session = sample.at("session").get<std::string>();
        if (session != activeSession) {
            GlobalVisualLocalizer::CancelPending();
            activeSession = session; ++sessionId; confirmation.Reset(); trusted.reset();
            if (usesResumeHints) {
                const auto now = MinimapResumePolicy::Clock::now();
                const auto parseHint = [&](const char* name, MinimapResumeSource source) {
                    const auto& value = manifest.at("resumeHints").at(name);
                    return MinimapResumeHint{source,
                        {value.at("sceneId").get<int>(), value.at("mapX").get<double>(), value.at("mapY").get<double>()},
                        now - std::chrono::milliseconds(value.value("ageMilliseconds", 0)), sessionId, sessionId};
                };
                resumePolicy.Begin(sessionId, parseHint("trusted", MinimapResumeSource::TrustedMinimap),
                    parseHint("viewport", MinimapResumeSource::MapViewport), sessionId, sessionId, now);
            }
        }
        const auto loaded = cv::imread((root / sample.at("image").get<std::string>()).string(), cv::IMREAD_UNCHANGED);
        if (loaded.empty()) {
            ++failed; results.push_back({{"id", sample.at("id")}, {"missing", true}}); continue;
        }
        const auto image = ApplyTransform(loaded, sample);
        cv::Mat normalized;
        ImageFeatureData features;
        const bool hasFeatures = GlobalVisualLocalizer::PrepareMinimap(image, normalized, features);
        const bool mustReject = sample.value("mustReject", false);
        const int maxFrames = std::clamp(sample.value("maxRecoveryFrames", 1), 1, 128);
        std::size_t sampleMaximumSearchOffset = 0;
        nlohmann::json attempts = nlohmann::json::array();
        bool supported = false, published = false, usedTracking = false, samplePassed = true;
        double trackingTerrainScale = 194.0 / 184.0;
        bool trackingScalePreserved = false;
        VisualLocalizationCandidate candidate;
        const auto start = std::chrono::steady_clock::now();
        if (hasFeatures && trusted) {
            // Match App: freeze the scale from the independently confirmed
            // acquisition, then preserve it through every local tracking frame.
            trackingTerrainScale = trusted->scale;
            usedTracking = GlobalVisualLocalizer::TrackLocal(normalized, features,
                trusted->sceneId, trusted->mapCenter, candidate, trackingTerrainScale) &&
                HasReacquisitionSupport(candidate.affineEstimated, candidate.inlierCount,
                    candidate.inlierRatio, candidate.coveredQuadrants);
            if (usedTracking) {
                supported = true; published = true; ++tracked;
                trackingScalePreserved = std::isfinite(candidate.scale) &&
                    std::abs(candidate.scale - trackingTerrainScale) < 1e-9;
                if (!trackingScalePreserved) samplePassed = false;
            }
            else trusted.reset();
        }
        if (hasFeatures && !usedTracking && usesResumeHints) {
            for (int index = 0; index < maxFrames; ++index) {
                const auto attempt = resumePolicy.NextAttempt(sessionId, ++frameId);
                if (!attempt) break;
                const auto searchStart = std::chrono::steady_clock::now();
                supported = GlobalVisualLocalizer::TrackNearby(normalized, features,
                    attempt->hint.position.sceneId,
                    {attempt->hint.position.x, attempt->hint.position.y}, 256.0, candidate) &&
                    HasReacquisitionSupport(candidate.affineEstimated, candidate.inlierCount,
                        candidate.inlierRatio, candidate.coveredQuadrants);
                published = resumePolicy.Observe(*attempt, supported,
                    {candidate.sceneId, candidate.mapCenter.x, candidate.mapCenter.y});
                const double elapsed = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - searchStart).count();
                attempts.push_back({{"frameId", frameId}, {"supported", supported}, {"published", published},
                    {"hintSource", MinimapResumePolicy::SourceName(attempt->hint.source)},
                    {"hintAttempt", attempt->ordinal}, {"totalMilliseconds", elapsed}});
                if (elapsed > sample.value("maxSearchMilliseconds", 500.0)) samplePassed = false;
                if (supported) { ++recovered; if (published) ++confirmed; break; }
            }
        }
        if (hasFeatures && !usedTracking && !usesResumeHints) {
            for (int attempt = 0; attempt < maxFrames; ++attempt) {
                VisualLocalizationRequest request;
                request.sessionId = sessionId; request.uiGeneration = sessionId;
                request.frameId = ++frameId; request.requestId = frameId;
                request.normalizedMinimap = normalized; request.minimapFeatures = features;
                if (!GlobalVisualLocalizer::Submit(std::move(request))) {
                    std::cerr << "Recovery submission failed.\n";
                    GlobalVisualLocalizer::Shutdown(); return 1;
                }
                VisualLocalizationResult result;
                bool received = false;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while (std::chrono::steady_clock::now() < deadline) {
                    if (GlobalVisualLocalizer::TryTakeLatestResult(result)) { received = true; break; }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                if (!received || result.sessionId != sessionId || result.frameId != frameId ||
                    result.requestId != frameId || result.uiGeneration != sessionId) {
                    std::cerr << "Recovery timed out or returned an obsolete frame.\n";
                    GlobalVisualLocalizer::Shutdown(); return 1;
                }
                maximumSearchOffset = std::max(maximumSearchOffset, result.coarseSearchOffset);
                sampleMaximumSearchOffset = std::max(sampleMaximumSearchOffset, result.coarseSearchOffset);
                if (!result.ambiguous && result.quality != VisualLocalizationQuality::Rejected &&
                    !result.candidates.empty()) {
                    const auto& global = result.candidates.front();
                    supported = GlobalVisualLocalizer::TrackNearby(normalized, features,
                        global.sceneId, global.mapCenter, 64.0, candidate) &&
                        HasReacquisitionSupport(candidate.affineEstimated, candidate.inlierCount,
                            candidate.inlierRatio, candidate.coveredQuadrants);
                }
                attempts.push_back({{"frameId", frameId}, {"supported", supported},
                    {"quality", GlobalVisualLocalizer::QualityName(result.quality)},
                    {"searchIncomplete", result.searchIncomplete}, {"totalMilliseconds", result.totalMilliseconds},
                    {"coarseSearchOffset", result.coarseSearchOffset},
                    {"totalCoarseCandidates", result.totalCoarseCandidates},
                    {"verifiedCoarseCandidates", result.verifiedCoarseCandidates},
                    {"nextCoarseSearchOffset", result.nextCoarseSearchOffset},
                    {"usedCandidateHint", result.usedCandidateHint},
                    {"attemptedRecoveryAngle", result.attemptedRecoveryAngle}});
                if (result.totalMilliseconds > sample.value("maxSearchMilliseconds", 500.0)) samplePassed = false;
                if (supported) { ++recovered; break; }
            }
        }
        // Repeated submissions of one screenshot exercise search progression
        // only. Publication requires the next distinct captured image entry.
        if (supported && !usedTracking && !usesResumeHints) {
            published = confirmation.Observe(sessionId, captureId,
                {candidate.sceneId, candidate.mapCenter.x, candidate.mapCenter.y});
            if (published) ++confirmed;
        }
        if (!supported) confirmation.Reset();
        if (published) trusted = candidate;
        bool consistencyMatched = false;
        double consistencyDistance = 0.0;
        if (supported && sample.contains("consistencyExpected")) {
            const auto& reference = sample.at("consistencyExpected");
            consistencyDistance = Distance(candidate.mapCenter,
                {reference.at("mapX").get<double>(), reference.at("mapY").get<double>()});
            consistencyMatched = candidate.sceneId == reference.at("sceneId").get<int>() &&
                consistencyDistance <= reference.value("tolerance", 12.0);
            if (!consistencyMatched) samplePassed = false;
        }
        if (mustReject) { ++negatives; if (supported || published) samplePassed = false; }
        if (sample.value("mustRecover", false) && !supported) samplePassed = false;
        if (sample.value("mustPublish", false) && !published) samplePassed = false;
        if (sample.value("mustTrack", false) && !usedTracking) samplePassed = false;
        if (sample.value("mustDeferPublication", false) && published) samplePassed = false;
        if (sample.value("mustContinueSearch", false) && sampleMaximumSearchOffset == 0) samplePassed = false;
        if (sample.contains("expectedHintSources")) {
            nlohmann::json sources = nlohmann::json::array();
            for (const auto& attempt : attempts) sources.push_back(attempt.value("hintSource", std::string{}));
            if (sources != sample.at("expectedHintSources")) samplePassed = false;
        }
        const double duration = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        if (duration > sample.value("maxRecoveryMilliseconds", 10000.0)) samplePassed = false;
        if (!samplePassed) ++failed;
        results.push_back({{"id", sample.at("id")}, {"scenarioPassed", samplePassed},
            {"featureless", !hasFeatures}, {"supported", supported}, {"published", published},
            {"tracked", usedTracking}, {"mustReject", mustReject},
            {"trackingTerrainScale", usedTracking ? nlohmann::json(trackingTerrainScale) : nlohmann::json(nullptr)},
            {"trackingScalePreserved", usedTracking ? nlohmann::json(trackingScalePreserved) : nlohmann::json(nullptr)},
            {"maximumSearchOffset", sampleMaximumSearchOffset},
            {"consistencyMatched", consistencyMatched}, {"consistencyDistance", consistencyDistance},
            {"candidate", supported ? SerializeCandidate(candidate) : nlohmann::json(nullptr)},
            {"durationMilliseconds", duration}, {"attempts", attempts}});
    }
    GlobalVisualLocalizer::Shutdown();
    if (manifest.value("requireSearchProgression", false) && maximumSearchOffset == 0) ++failed;
    const nlohmann::json report = {{"scenarioPassed", failed == 0}, {"failed", failed},
        {"provenance", manifest.at("provenance")}, {"independentGroundTruth", false},
        {"accuracyEvaluated", false}, {"usesResumeHints", usesResumeHints},
        {"recovered", recovered}, {"confirmed", confirmed},
        {"tracked", tracked}, {"negativeSamples", negatives},
        {"maximumSearchOffset", maximumSearchOffset}, {"samples", results}};
    std::filesystem::create_directories(reportPath.parent_path());
    std::ofstream(reportPath) << report.dump(2) << '\n';
    std::cout << report.dump(2) << '\n';
    return failed == 0 ? 0 : 3;
}

__declspec(noinline) int ReplayViewports(const std::filesystem::path& root, const nlohmann::json& manifest,
    const std::filesystem::path& reportPath, std::shared_ptr<RuntimeFeatureResources> resources) {
    std::string error;
    if (!MapViewportLocalizer::Initialize(resources, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    nlohmann::json results = nlohmann::json::array();
    int failed = 0;
    std::uint64_t requestId = 0;
    for (const auto& sample : manifest.at("samples")) {
        const auto path = root / sample.at("image").get<std::string>();
        auto image = cv::imread(path.string());
        if (image.empty()) { ++failed; results.push_back({{"id", sample.at("id")}, {"missing", true}}); continue; }
        image = ApplyTransform(image, sample);
        RECT rect{0, 0, image.cols, image.rows};
        MapViewportLocalizationRequest request;
        request.requestId = ++requestId;
        request.mapCrop = ImageProcessing::CropToMapCenterArea(image, rect);
        MapViewportLocalizer::Submit(std::move(request));
        MapViewportLocalizationResult result;
        bool received = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < deadline) {
            if (MapViewportLocalizer::TryTakeLatestResult(result) && result.requestId == requestId) { received = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        bool correct = received && (sample.value("mustReject", false) ? !result.accepted : result.accepted);
        double distance = 0.0;
        if (sample.contains("expected") && result.accepted) {
            const auto& expected = sample.at("expected");
            distance = std::hypot(result.centerMapCoordinate.x - expected.at("mapX").get<double>(),
                result.centerMapCoordinate.y - expected.at("mapY").get<double>());
            correct = correct && distance <= expected.value("tolerance", 12.0);
        }
        if (!correct) ++failed;
        results.push_back({{"id", sample.at("id")}, {"accepted", result.accepted}, {"correct", correct},
            {"matches", result.goodMatchCount}, {"inliers", result.inlierCount},
            {"mapX", result.centerMapCoordinate.x}, {"mapY", result.centerMapCoordinate.y},
            {"errorDistance", distance}, {"durationMs", result.durationMilliseconds}});
    }
    MapViewportLocalizer::Shutdown();
    std::filesystem::create_directories(reportPath.parent_path());
    const nlohmann::json report = {{"failed", failed}, {"samples", results}};
    std::ofstream(reportPath) << report.dump(2) << '\n';
    std::cout << report.dump(2) << '\n';
    return failed == 0 ? 0 : 3;
}
}

int wmain(int argumentCount, wchar_t** arguments) {
    if (argumentCount != 4) {
        std::wcerr << L"Usage: IMaoVisualRegression <repo-root> <manifest.json> <report.json>\n";
        return 2;
    }
    const std::filesystem::path repositoryRoot(arguments[1]);
    const std::filesystem::path manifestPath(arguments[2]);
    const std::filesystem::path reportPath(arguments[3]);
    std::ifstream manifestFile(manifestPath);
    if (!manifestFile) {
        std::cerr << "Visual regression manifest cannot be opened.\n";
        return 1;
    }
    const auto manifest = nlohmann::json::parse(manifestFile);
    if (!manifest.contains("samples") || !manifest.at("samples").is_array() || manifest.at("samples").empty()) {
        std::cerr << "Visual regression requires a non-empty sample list.\n";
        return 2;
    }
    std::string error;
    const auto loadStart = std::chrono::steady_clock::now();
    const auto workingSetBeforeLoad = WorkingSetBytes();
    const auto resources = LoadResources(repositoryRoot, error);
    if (!resources) {
        std::cerr << "Visual resources failed to load: " << error << '\n';
        return 1;
    }
    if (manifest.value("viewport", false)) return ReplayViewports(repositoryRoot, manifest, reportPath, resources);
    if (manifest.value("recoverySequence", false)) return ReplayRecoverySequence(repositoryRoot, manifest, reportPath, resources);
    const double resourceLoadMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - loadStart).count();
    const auto workingSetAfterLoad = WorkingSetBytes();
    if (!GlobalVisualLocalizer::Initialize(resources, error)) {
        std::cerr << "Visual localizer failed to initialize: " << error << '\n';
        return 1;
    }

    const int requiredPublications = static_cast<int>(std::count_if(manifest.at("samples").begin(), manifest.at("samples").end(),
        [](const auto& sample) { return sample.value("mustPublish", false); }));
    int requiredPublished = 0, latencyViolations = 0;
    int processed = 0;
    int missing = 0;
    int featureless = 0;
    int labeled = 0;
    int strong = 0;
    int labeledStrong = 0;
    int strongCorrect = 0;
    int rawStrong = 0;
    int labeledRawStrong = 0;
    int rawStrongCorrect = 0;
    int finalAccepted = 0;
    int labeledAccepted = 0;
    int unlabeledAccepted = 0;
    int finalAcceptedCorrect = 0;
    int falseAccepted = 0;
    int ambiguous = 0;
    std::vector<double> coarseTimes;
    std::vector<double> verificationTimes;
    std::vector<double> totalTimes;
    std::vector<double> acceptedTotalTimes;
    std::vector<double> preparationTimes;
    std::vector<double> localTrackingTimes;
    // This mirrors App's publisher policy: a Strong global result is a good
    // candidate, not a one-frame position.  Only a current OCR-bounded result
    // may publish immediately; all other global candidates need a consistent
    // next frame.
    std::unordered_map<std::string, PendingMarginal> pendingConfirmations;
    nlohmann::json samples = nlohmann::json::array();
    std::uint64_t frameId = 0;

    for (const auto& sample : manifest.at("samples")) {
        ++frameId;
        const auto imagePath = repositoryRoot / sample.at("image").get<std::string>();
        const auto loadedImage = cv::imread(imagePath.string(), cv::IMREAD_UNCHANGED);
        if (loadedImage.empty()) {
            ++missing;
            continue;
        }
        cv::Mat sampleImage = loadedImage;
        if (sample.value("fullSnapshot", false)) {
            Coordinate minimapBottomPoint;
            const RECT clientRect{ 0, 0, sampleImage.cols, sampleImage.rows };
            sampleImage = ImageProcessing::CropToMinMapAreaImg(sampleImage, clientRect, minimapBottomPoint);
        }
        const auto image = ApplyTransform(sampleImage, sample);
        cv::Mat trustedReference;
        if (sample.contains("trustedReferenceImage") && !sample.at("trustedReferenceImage").is_null()) {
            const auto referencePath = repositoryRoot /
                sample.at("trustedReferenceImage").get<std::string>();
            cv::Mat referenceImage = cv::imread(referencePath.string(), cv::IMREAD_UNCHANGED);
            if (!referenceImage.empty()) {
                if (sample.value("trustedReferenceFullSnapshot", false)) {
                    Coordinate minimapBottomPoint;
                    const RECT clientRect{ 0, 0, referenceImage.cols, referenceImage.rows };
                    referenceImage = ImageProcessing::CropToMinMapAreaImg(referenceImage, clientRect,
                        minimapBottomPoint);
                }
                if (!referenceImage.empty()) {
                    cv::resize(referenceImage, trustedReference, cv::Size(184, 184), 0.0, 0.0,
                        cv::INTER_AREA);
                }
            }
        }
        ++processed;
        cv::Mat normalized;
        ImageFeatureData features;
        MinimapFeatureDiagnostics minimapDiagnostics;
        const auto preparationStart = std::chrono::steady_clock::now();
        // A fixture can supply the previous trusted minimap frame, exercising
        // the same dynamic-region rejection used by the runtime. Independent
        // samples intentionally use the fixed player/heading exclusion only.
        if (!GlobalVisualLocalizer::PrepareMinimap(image, normalized, features,
                trustedReference.empty() ? nullptr : &trustedReference, &minimapDiagnostics)) {
            ++featureless;
            samples.push_back({
                {"id", sample.at("id")}, {"image", sample.at("image")},
                {"labeled", sample.contains("expected") && !sample.at("expected").is_null()},
                {"mustReject", sample.value("mustReject", false)}, {"featureless", true},
                {"quality", "Rejected"}, {"published", false}, {"correct", sample.value("mustReject", false)}
            });
            continue;
        }
        const double preparationMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - preparationStart).count();
        preparationTimes.push_back(preparationMilliseconds);
        VisualLocalizationRequest request;
        request.sessionId = 1;
        request.uiGeneration = 1;
        request.frameId = frameId;
        request.requestId = frameId;
        request.normalizedMinimap = normalized;
        request.minimapFeatures = features;
        if (sample.contains("worldHint") && !sample.at("worldHint").is_null()) {
            const auto& worldHint = sample.at("worldHint");
            const int hintSceneId = sample.value("hintSceneId", 1);
            const Coordinate worldCoordinate{
                worldHint.at("x").get<double>(), worldHint.at("y").get<double>() };
            request.ocrHints.push_back({ hintSceneId,
                MapCoordinate::IdentifyCoorToImgMapCoord(worldCoordinate, hintSceneId) });
            request.requireOcrHint = true;
        }
        if (sample.value("cancelBeforeSubmit", false)) {
            auto obsolete = request;
            obsolete.sessionId = 9999; obsolete.requestId = 9999;
            obsolete.minimapFeatures.imgDescriptors = request.minimapFeatures.imgDescriptors.clone();
            cv::RNG random(20260907);
            random.fill(obsolete.minimapFeatures.imgDescriptors, cv::RNG::UNIFORM, 0.0, 1.0);
            if (!GlobalVisualLocalizer::Submit(std::move(obsolete))) return 1;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            GlobalVisualLocalizer::CancelPending();
            VisualLocalizationResult cancelled;
            if (GlobalVisualLocalizer::TryTakeLatestResult(cancelled)) {
                std::cerr << "Cancelled request remained visible.\n";
                GlobalVisualLocalizer::Shutdown(); return 1;
            }
        }
        if (!GlobalVisualLocalizer::Submit(std::move(request))) {
            std::cerr << "Visual request submission failed.\n";
            GlobalVisualLocalizer::Shutdown();
            return 1;
        }

        VisualLocalizationResult result;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        bool received = false;
        while (std::chrono::steady_clock::now() < deadline) {
            if (GlobalVisualLocalizer::TryTakeLatestResult(result)) {
                if (result.frameId != frameId || result.requestId != frameId || result.sessionId != 1) {
                    std::cerr << "Obsolete request published after cancellation or replacement.\n";
                    GlobalVisualLocalizer::Shutdown(); return 1;
                }
                received = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!received) {
            std::cerr << "Visual request timed out at frame " << frameId << ".\n";
            GlobalVisualLocalizer::Shutdown();
            return 1;
        }
        coarseTimes.push_back(result.coarseMilliseconds);
        verificationTimes.push_back(result.verificationMilliseconds);
        totalTimes.push_back(result.totalMilliseconds);
        const double sampleBudget = sample.value("maxSearchMilliseconds", 0.0);
        if (sampleBudget > 0.0 && result.totalMilliseconds > sampleBudget) ++latencyViolations;
        if (result.ambiguous) ++ambiguous;
        double localTrackingMilliseconds = 0.0;
        bool localTrackingAccepted = false;
        VisualLocalizationCandidate localTrackingCandidate;
        if (!result.ambiguous && !result.candidates.empty() &&
            result.quality != VisualLocalizationQuality::Rejected) {
            const auto trackingStart = std::chrono::steady_clock::now();
            // App revalidates every global candidate before publication. Test
            // the same nearby path, including fixed-scale translation votes.
            localTrackingAccepted = GlobalVisualLocalizer::TrackNearby(normalized, features,
                result.candidates.front().sceneId, result.candidates.front().mapCenter, 64.0,
                localTrackingCandidate);
            localTrackingMilliseconds = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - trackingStart).count();
            localTrackingTimes.push_back(localTrackingMilliseconds);
        }

        const bool hasExpected = sample.contains("expected") && !sample.at("expected").is_null();
        const bool mustReject = sample.value("mustReject", false);
        const bool requiresOcrHintVerification = sample.value("requiresOcrHintVerification", false);
        int expectedScene = 0;
        Coordinate expected;
        double tolerance = 24.0;
        if (hasExpected) {
            ++labeled;
            const auto& expectedJson = sample.at("expected");
            expectedScene = expectedJson.at("sceneId").get<int>();
            expected.x = expectedJson.at("mapX").get<double>();
            expected.y = expectedJson.at("mapY").get<double>();
            tolerance = expectedJson.value("tolerance", 24.0);
        }

        double localTrackingErrorDistance = 0.0;
        bool localTrackingCorrect = false;
        if (hasExpected && localTrackingAccepted) {
            localTrackingErrorDistance = Distance(localTrackingCandidate.mapCenter, expected);
            localTrackingCorrect = localTrackingCandidate.sceneId == expectedScene &&
                localTrackingErrorDistance <= tolerance;
        }

        const VisualLocalizationCandidate* published = nullptr;
        bool requiresSecondFrame = false;
        const std::string session = sample.value("session", std::string{});
        if (!result.ambiguous && !result.candidates.empty() &&
            (result.quality == VisualLocalizationQuality::Strong ||
                result.quality == VisualLocalizationQuality::Marginal) &&
            localTrackingAccepted && HasReacquisitionSupport(localTrackingCandidate.affineEstimated,
                localTrackingCandidate.inlierCount, localTrackingCandidate.inlierRatio,
                localTrackingCandidate.coveredQuadrants)) {
            localTrackingCandidate.ocrHintMatched = result.candidates.front().ocrHintMatched;
            const auto& candidate = localTrackingCandidate;
            const bool isStrong = candidate.quality == VisualLocalizationQuality::Strong;
            if (isStrong) ++rawStrong;
            // Marginal matches always require a confirmation.  Strong matches
            // can publish in one frame only when an OCR hint selected the
            // region and image geometry has already confirmed that hint.
            requiresSecondFrame = !isStrong || !candidate.ocrHintMatched;
            if (!requiresSecondFrame) {
                published = &candidate;
                pendingConfirmations.erase(session);
            }
            else {
                const auto previous = pendingConfirmations.find(session);
                if (previous != pendingConfirmations.end() && previous->second.frameId + 1 == frameId &&
                    previous->second.candidate.sceneId == candidate.sceneId &&
                    Distance(previous->second.candidate.mapCenter, candidate.mapCenter) <= 12.0) {
                    published = &candidate;
                    pendingConfirmations.erase(previous);
                }
                else {
                    pendingConfirmations[session] = { frameId, candidate };
                }
            }
        }
        else {
            pendingConfirmations.erase(session);
        }

        bool correct = false;
        double errorDistance = 0.0;
        if (published != nullptr) {
            ++finalAccepted;
            acceptedTotalTimes.push_back(result.totalMilliseconds);
            if (hasExpected) {
                ++labeledAccepted;
                errorDistance = Distance(published->mapCenter, expected);
                correct = published->sceneId == expectedScene && errorDistance <= tolerance;
                if (correct) ++finalAcceptedCorrect;
                else ++falseAccepted;
            }
            else {
                ++unlabeledAccepted;
            }
            if (mustReject) ++falseAccepted;
            if (requiresOcrHintVerification && !published->ocrHintMatched) ++falseAccepted;
        }
        if (hasExpected && result.quality == VisualLocalizationQuality::Strong &&
            !result.ambiguous && !result.candidates.empty()) {
            ++labeledRawStrong;
            const auto& top = result.candidates.front();
            if (top.sceneId == expectedScene && Distance(top.mapCenter, expected) <= tolerance) ++rawStrongCorrect;
        }
        if (sample.value("mustPublish", false) && published != nullptr && correct) ++requiredPublished;
        if (published != nullptr && result.quality == VisualLocalizationQuality::Strong) {
            ++strong;
            if (hasExpected) {
                ++labeledStrong;
                if (published->sceneId == expectedScene && Distance(published->mapCenter, expected) <= tolerance) {
                    ++strongCorrect;
                }
            }
        }

        nlohmann::json candidates = nlohmann::json::array();
        for (const auto& candidate : result.candidates) candidates.push_back(SerializeCandidate(candidate));
        samples.push_back({
            {"id", sample.at("id")}, {"image", sample.at("image")},
            {"labeled", hasExpected}, {"mustReject", mustReject},
            {"requiresOcrHintVerification", requiresOcrHintVerification},
            {"quality", GlobalVisualLocalizer::QualityName(result.quality)},
            {"ambiguous", result.ambiguous}, {"searchIncomplete", result.searchIncomplete},
            {"recoveryAngle", result.recoveryAngle}, {"cancellationVerified", sample.value("cancelBeforeSubmit", false)}, {"published", published != nullptr},
            {"requiresSecondFrame", requiresSecondFrame}, {"correct", correct},
            {"errorDistance", errorDistance}, {"coarseMilliseconds", result.coarseMilliseconds},
            {"preparationMilliseconds", preparationMilliseconds},
            {"minimapRawKeypoints", minimapDiagnostics.rawKeypointCount},
            {"minimapRetainedKeypoints", minimapDiagnostics.retainedKeypointCount},
            {"minimapDynamicMaskPercent", minimapDiagnostics.dynamicMaskPercent},
            {"minimapTemporalMaskApplied", minimapDiagnostics.temporalMaskApplied},
            {"verificationMilliseconds", result.verificationMilliseconds},
            {"coarseCandidateCount", result.coarseCandidateCount},
            {"bestMutualMatchCount", result.bestMutualMatchCount},
            {"bestInlierCount", result.bestInlierCount},
            {"bestObservedScale", result.bestObservedScale},
            {"bestAffineEstimated", result.bestAffineEstimated},
            {"bestScaleWithinExpectedRange", result.bestScaleWithinExpectedRange},
            {"bestRetrievalScore", result.bestRetrievalScore},
            {"totalMilliseconds", result.totalMilliseconds},
            {"localTrackingMilliseconds", localTrackingMilliseconds},
            {"localTrackingAccepted", localTrackingAccepted},
            {"localTrackingMapX", localTrackingCandidate.mapCenter.x},
            {"localTrackingMapY", localTrackingCandidate.mapCenter.y},
            {"localTrackingErrorDistance", localTrackingErrorDistance},
            {"localTrackingCorrect", localTrackingCorrect}, {"candidates", candidates}
        });
    }
    GlobalVisualLocalizer::Shutdown();

    const double acceptedGlobalP95 = Percentile(acceptedTotalTimes, 0.95);
    const double localTrackingP95 = Percentile(localTrackingTimes, 0.95);
    const double acceptedStrongPrecision = labeledStrong > 0
        ? static_cast<double>(strongCorrect) / labeledStrong : 0.0;
    const double rawStrongPrecision = labeledRawStrong > 0
        ? static_cast<double>(rawStrongCorrect) / labeledRawStrong : 0.0;
    // Publication is intentionally allowed for a Marginal candidate only
    // after an independent second frame confirms it.  Evaluate the safety of
    // that actual publication policy rather than requiring every accepted
    // result to originate from a one-frame Strong estimate.
    const double acceptedPrecision = labeledAccepted > 0
        ? static_cast<double>(finalAcceptedCorrect) / labeledAccepted
        : (labeled == 0 ? 1.0 : 0.0);
    const bool acceptancePassed = missing == 0 && processed > 0 && labeled > 0 &&
        requiredPublished == requiredPublications && latencyViolations == 0 &&
        falseAccepted == 0 && acceptedPrecision >= 0.95 &&
        acceptedGlobalP95 <= 250.0 && localTrackingP95 <= 50.0;
    const nlohmann::json report = {
        {"requiredPublications", requiredPublications}, {"requiredPublished", requiredPublished},
        {"latencyViolations", latencyViolations},
        {"processed", processed}, {"missing", missing}, {"featureless", featureless},
        {"labeled", labeled}, {"strong", strong}, {"labeledStrong", labeledStrong},
        {"strongCorrect", strongCorrect},
        {"strongPrecision", acceptedStrongPrecision},
        {"rawStrong", rawStrong}, {"labeledRawStrong", labeledRawStrong},
        {"rawStrongCorrect", rawStrongCorrect},
        {"rawStrongPrecision", rawStrongPrecision},
        {"acceptedPrecision", acceptedPrecision},
        {"acceptancePassed", acceptancePassed},
        {"finalAccepted", finalAccepted}, {"labeledAccepted", labeledAccepted},
        {"unlabeledAccepted", unlabeledAccepted}, {"finalAcceptedCorrect", finalAcceptedCorrect},
        {"falseAccepted", falseAccepted}, {"ambiguousRejected", ambiguous},
        {"resourceLoadMilliseconds", resourceLoadMilliseconds},
        {"additionalWorkingSetBytes", workingSetAfterLoad >= workingSetBeforeLoad
            ? workingSetAfterLoad - workingSetBeforeLoad : 0},
        {"preparationP50Milliseconds", Percentile(preparationTimes, 0.50)},
        {"preparationP95Milliseconds", Percentile(preparationTimes, 0.95)},
        {"coarseP50Milliseconds", Percentile(coarseTimes, 0.50)},
        {"coarseP95Milliseconds", Percentile(coarseTimes, 0.95)},
        {"verificationP50Milliseconds", Percentile(verificationTimes, 0.50)},
        {"verificationP95Milliseconds", Percentile(verificationTimes, 0.95)},
        {"globalP50Milliseconds", Percentile(totalTimes, 0.50)},
        {"globalP95Milliseconds", Percentile(totalTimes, 0.95)},
        {"acceptedGlobalP95Milliseconds", acceptedGlobalP95},
        {"localTrackingP50Milliseconds", Percentile(localTrackingTimes, 0.50)},
        {"localTrackingP95Milliseconds", localTrackingP95},
        {"samples", samples}
    };
    std::filesystem::create_directories(reportPath.parent_path());
    std::ofstream output(reportPath);
    output << report.dump(2) << '\n';
    std::cout << report.dump(2) << '\n';
    return acceptancePassed ? 0 : 3;
}
