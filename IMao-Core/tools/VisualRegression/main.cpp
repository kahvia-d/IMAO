#include "Coordinate/VisualLocalization/GlobalVisualLocalizer.h"
#include "Feature/CandidateFeaturePack.h"
#include "Feature/KuroTileFeaturePack.h"
#include "Feature/Processing/FeatureBinaryCodec.h"
#include "Feature/VisualIndex/MapVisualIndex.h"

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
        {"ocrHintMatched", candidate.ocrHintMatched},
        {"quality", GlobalVisualLocalizer::QualityName(candidate.quality)}
    };
}

cv::Mat ApplyTransform(const cv::Mat& source, const nlohmann::json& sample) {
    if (!sample.contains("transform") || sample.at("transform").is_null()) return source;
    const auto& transform = sample.at("transform");
    cv::Mat output = source.clone();
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
    std::string error;
    const auto loadStart = std::chrono::steady_clock::now();
    const auto workingSetBeforeLoad = WorkingSetBytes();
    const auto resources = LoadResources(repositoryRoot, error);
    if (!resources) {
        std::cerr << "Visual resources failed to load: " << error << '\n';
        return 1;
    }
    const double resourceLoadMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - loadStart).count();
    const auto workingSetAfterLoad = WorkingSetBytes();
    if (!GlobalVisualLocalizer::Initialize(resources, error)) {
        std::cerr << "Visual localizer failed to initialize: " << error << '\n';
        return 1;
    }

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
        const auto image = ApplyTransform(loadedImage, sample);
        ++processed;
        cv::Mat normalized;
        ImageFeatureData features;
        const auto preparationStart = std::chrono::steady_clock::now();
        if (!GlobalVisualLocalizer::PrepareMinimap(image, normalized, features)) {
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
        request.normalizedMinimap = normalized;
        request.minimapFeatures = features;
        if (!GlobalVisualLocalizer::Submit(std::move(request))) {
            std::cerr << "Visual request submission failed.\n";
            GlobalVisualLocalizer::Shutdown();
            return 1;
        }

        VisualLocalizationResult result;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        bool received = false;
        while (std::chrono::steady_clock::now() < deadline) {
            if (GlobalVisualLocalizer::TryTakeLatestResult(result) && result.frameId == frameId) {
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
        if (result.ambiguous) ++ambiguous;
        double localTrackingMilliseconds = 0.0;
        bool localTrackingAccepted = false;
        if (!result.ambiguous && !result.candidates.empty() &&
            result.quality == VisualLocalizationQuality::Strong) {
            VisualLocalizationCandidate tracked;
            const auto trackingStart = std::chrono::steady_clock::now();
            localTrackingAccepted = GlobalVisualLocalizer::TrackLocal(normalized, features,
                result.candidates.front().sceneId, result.candidates.front().mapCenter, tracked);
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

        const VisualLocalizationCandidate* published = nullptr;
        bool requiresSecondFrame = false;
        const std::string session = sample.value("session", std::string{});
        if (!result.ambiguous && !result.candidates.empty() &&
            (result.quality == VisualLocalizationQuality::Strong ||
                result.quality == VisualLocalizationQuality::Marginal)) {
            const auto& candidate = result.candidates.front();
            const bool isStrong = result.quality == VisualLocalizationQuality::Strong;
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
            {"ambiguous", result.ambiguous}, {"published", published != nullptr},
            {"requiresSecondFrame", requiresSecondFrame}, {"correct", correct},
            {"errorDistance", errorDistance}, {"coarseMilliseconds", result.coarseMilliseconds},
            {"preparationMilliseconds", preparationMilliseconds},
            {"verificationMilliseconds", result.verificationMilliseconds},
            {"bestRetrievalScore", result.bestRetrievalScore},
            {"totalMilliseconds", result.totalMilliseconds},
            {"localTrackingMilliseconds", localTrackingMilliseconds},
            {"localTrackingAccepted", localTrackingAccepted}, {"candidates", candidates}
        });
    }
    GlobalVisualLocalizer::Shutdown();

    const double acceptedGlobalP95 = Percentile(acceptedTotalTimes, 0.95);
    const double localTrackingP95 = Percentile(localTrackingTimes, 0.95);
    const double acceptedStrongPrecision = labeledStrong > 0
        ? static_cast<double>(strongCorrect) / labeledStrong : 0.0;
    const double rawStrongPrecision = labeledRawStrong > 0
        ? static_cast<double>(rawStrongCorrect) / labeledRawStrong : 0.0;
    const bool acceptancePassed = falseAccepted == 0 && acceptedStrongPrecision >= 0.95 &&
        acceptedGlobalP95 <= 250.0 && localTrackingP95 <= 50.0;
    const nlohmann::json report = {
        {"processed", processed}, {"missing", missing}, {"featureless", featureless},
        {"labeled", labeled}, {"strong", strong}, {"labeledStrong", labeledStrong},
        {"strongCorrect", strongCorrect},
        {"strongPrecision", acceptedStrongPrecision},
        {"rawStrong", rawStrong}, {"labeledRawStrong", labeledRawStrong},
        {"rawStrongCorrect", rawStrongCorrect},
        {"rawStrongPrecision", rawStrongPrecision},
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
