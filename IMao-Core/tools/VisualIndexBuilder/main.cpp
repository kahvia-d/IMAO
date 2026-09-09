#include "Feature/CandidateFeaturePack.h"
#include "Feature/KuroTileFeaturePack.h"
#include "Feature/Match/FeatureMatch.h"
#include "Feature/Processing/FeatureBinaryCodec.h"
#include "Feature/VisualIndex/MapVisualIndex.h"

#include <nlohmann/json.hpp>
#include <windows.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
std::string Hex(const std::array<std::uint8_t, 32>& value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(64);
    for (const auto byte : value) {
        output.push_back(digits[byte >> 4]);
        output.push_back(digits[byte & 0x0f]);
    }
    return output;
}

bool ReplaceFile(const std::filesystem::path& temporary, const std::filesystem::path& destination,
    std::string& error) {
    if (MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    error = "MoveFileExW failed: " + std::to_string(GetLastError());
    return false;
}

bool Equivalent(const MapVisualIndex& left, const MapVisualIndex& right) {
    if (left.featureCount != right.featureCount || left.sourceImfSha256 != right.sourceImfSha256 ||
        left.vocabulary.size() != right.vocabulary.size() || left.vocabulary.type() != right.vocabulary.type() ||
        std::memcmp(left.vocabulary.data, right.vocabulary.data,
            left.vocabulary.total() * left.vocabulary.elemSize()) != 0 ||
        left.tiles.size() != right.tiles.size() || left.histograms.size() != right.histograms.size() ||
        left.featureRows != right.featureRows || left.postings.size() != right.postings.size()) return false;
    for (std::size_t index = 0; index < left.tiles.size(); ++index) {
        const auto& expected = left.tiles[index];
        const auto& actual = right.tiles[index];
        if (expected.sceneId != actual.sceneId || expected.gridX != actual.gridX ||
            expected.gridY != actual.gridY || expected.minX != actual.minX ||
            expected.minY != actual.minY || expected.maxX != actual.maxX ||
            expected.maxY != actual.maxY ||
            expected.histogramOffset != actual.histogramOffset ||
            expected.histogramCount != actual.histogramCount ||
            expected.featureRowOffset != actual.featureRowOffset ||
            expected.featureRowCount != actual.featureRowCount ||
            expected.histogramNorm != actual.histogramNorm) return false;
    }
    for (std::size_t index = 0; index < left.histograms.size(); ++index) {
        if (left.histograms[index].wordId != right.histograms[index].wordId ||
            left.histograms[index].weight != right.histograms[index].weight) return false;
    }
    for (std::size_t index = 0; index < left.postings.size(); ++index) {
        if (left.postings[index].wordId != right.postings[index].wordId ||
            left.postings[index].tileIndex != right.postings[index].tileIndex ||
            left.postings[index].weight != right.postings[index].weight) return false;
    }
    return true;
}

bool BuildAndInstallShard(const ImageFeatureData& features, const cv::Mat& vocabulary,
    const std::filesystem::path& sourcePath, const std::filesystem::path& destination,
    int sceneId, nlohmann::json& report, std::string& error) {
    std::array<std::uint8_t, 32> sourceHash{};
    if (!FeatureBinaryCodec::Sha256File(sourcePath, sourceHash, error)) return false;
    MapVisualIndex shard;
    if (!MapVisualIndexBuilder::Build(features.imgKeypoints, features.imgDescriptors,
        sourceHash, shard, error, vocabulary, sceneId)) return false;
    std::filesystem::create_directories(destination.parent_path());
    const auto temporary = destination.wstring() + L".tmp";
    MapVisualIndexHeader savedHeader;
    if (!MapVisualIndexCodec::Save(temporary, shard, error, &savedHeader)) return false;
    MapVisualIndex verified;
    if (!MapVisualIndexCodec::Load(temporary, sourceHash, shard.featureCount, verified, error) ||
        !Equivalent(shard, verified)) {
        if (error.empty()) error = "optional visual shard round-trip mismatch";
        return false;
    }
    std::array<std::uint8_t, 32> outputHash{};
    if (!FeatureBinaryCodec::Sha256File(temporary, outputHash, error) ||
        !ReplaceFile(temporary, destination, error)) return false;
    report = {
        {"path", destination.filename().string()}, {"sceneId", sceneId},
        {"source", sourcePath.filename().string()}, {"sourceSha256", Hex(sourceHash)},
        {"visualIndexSha256", Hex(outputHash)}, {"vocabularySha256", Hex(savedHeader.vocabularySha256)},
        {"featureCount", savedHeader.featureCount}, {"tileCount", savedHeader.tileCount},
        {"postingCount", savedHeader.postingCount}
    };
    return true;
}
}

int main(int argc, char** argv) {
    // Build one development shard with the existing vocabulary. Never rebuild
    // the baseline or change the pack's field-verification/approval metadata.
    if (argc == 5 && std::string(argv[1]) == "--pack-only") {
        try {
            const std::filesystem::path featureRoot = std::filesystem::path(argv[2]) / "FeaturesDatas";
            const std::filesystem::path packRoot(argv[3]);
            const bool allowUnverified = std::string(argv[4]) == "--allow-unverified";
            if (!allowUnverified && std::string(argv[4]) != "--verified") throw std::runtime_error("Expected --verified or --allow-unverified");
            std::ifstream input(packRoot / "manifest.json");
            const auto manifest = nlohmann::json::parse(input);
            if (manifest.value("formatVersion", 0) != 1) throw std::runtime_error("Unsupported pack format");
            const auto* scene = Scene::Find(manifest.at("sceneId").get<int>());
            if (!scene || manifest.at("scene").get<std::string>() != scene->name || manifest.at("source").at("state").get<int>() != scene->kuroStateId)
                throw std::runtime_error("Pack scene identity is invalid");
            if (!allowUnverified && !manifest.at("referenceVerification").value("passed", false))
                throw std::runtime_error("Pack has no verified reference");
            const auto file = manifest.at("features").at("file").get<std::string>();
            if (std::filesystem::path(file).filename() != file) throw std::runtime_error("Invalid feature filename");
            std::string error;
            ImageFeatureData base, features;
            FeatureBinaryHeader baseHeader, packHeader;
            std::array<std::uint8_t, 32> baseHash{}, xmlHash{};
            MapVisualIndex baseline;
            if (!FeatureBinaryCodec::Load(featureRoot / "Map_features.imf", base, error, &baseHeader, &baseHash) ||
                !MapVisualIndexCodec::Load(featureRoot / "Map_visual_index.imx", baseHash, baseHeader.keypointCount, baseline, error) ||
                !FeatureBinaryCodec::Load(packRoot / "features.imf", features, error, &packHeader) ||
                !FeatureBinaryCodec::Sha256File(packRoot / file, xmlHash, error)) throw std::runtime_error(error);
            if (xmlHash != packHeader.sourceXmlSha256 || Hex(xmlHash) != manifest.at("features").at("sha256").get<std::string>() ||
                packHeader.keypointCount != manifest.at("features").at("keypointCount").get<std::uint32_t>())
                throw std::runtime_error("Pack binary and source manifest disagree");
            nlohmann::json report;
            if (!BuildAndInstallShard(features, baseline.vocabulary, packRoot / file,
                packRoot / "visual-index.imx", scene->id, report, error)) throw std::runtime_error(error);
            report["referenceVerified"] = manifest.at("referenceVerification").value("passed", false);
            std::ofstream(packRoot / "visual-index.manifest.json") << report.dump(2) << '\n';
            std::cout << report.dump(2) << '\n';
            return 0;
        } catch (const std::exception& exception) {
            std::cerr << exception.what() << '\n';
            return 1;
        }
    }
    if (argc < 3 || argc > 4) {
        std::cerr << "Usage: IMaoVisualIndexBuilder <Assets directory> <output.imx> [manifest.json]\n";
        return 2;
    }
    const std::filesystem::path assetRoot = std::filesystem::absolute(argv[1]);
    const std::filesystem::path featureRoot = assetRoot / "FeaturesDatas";
    const std::filesystem::path sourceImf = featureRoot / "Map_features.imf";
    const std::filesystem::path destination = std::filesystem::absolute(argv[2]);
    const std::filesystem::path manifest = argc == 4
        ? std::filesystem::absolute(argv[3])
        : destination.parent_path() / "Map_visual_index.manifest.json";
    const auto temporary = destination.wstring() + L".tmp";
    const auto temporaryManifest = manifest.wstring() + L".tmp";

    std::string error;
    ImageFeatureData features;
    FeatureBinaryHeader featureHeader;
    std::array<std::uint8_t, 32> sourceImfSha{};
    if (!FeatureBinaryCodec::Load(sourceImf, features, error, &featureHeader, &sourceImfSha)) {
        std::cerr << "Unable to load source IMF: " << error << '\n';
        return 1;
    }

    const auto kuroPacks = KuroTileFeaturePack::LoadRegistered(featureRoot.string());
    const auto candidates = CandidateFeaturePack::LoadRegisteredCandidates(featureRoot.string());

    const auto start = std::chrono::steady_clock::now();
    MapVisualIndex visualIndex;
    // Preserve the legacy nearest-origin partitioning here: it is part of the
    // retrieval index and changing it changes ranking.  At runtime, successful
    // base-index matches are reported as World (see GlobalVisualLocalizer).
    if (!MapVisualIndexBuilder::Build(features.imgKeypoints, features.imgDescriptors,
            sourceImfSha, visualIndex, error)) {
        std::cerr << "Unable to build visual index: " << error << '\n';
        return 1;
    }
    MapVisualIndexHeader savedHeader;
    if (!MapVisualIndexCodec::Save(temporary, visualIndex, error, &savedHeader)) {
        std::cerr << "Unable to save visual index: " << error << '\n';
        return 1;
    }
    MapVisualIndex verified;
    MapVisualIndexHeader verifiedHeader;
    if (!MapVisualIndexCodec::Load(temporary, sourceImfSha, visualIndex.featureCount,
            verified, error, &verifiedHeader) || !Equivalent(visualIndex, verified)) {
        std::cerr << "Visual index round-trip verification failed: " << error << '\n';
        return 1;
    }

    std::array<std::uint8_t, 32> outputSha{};
    if (!FeatureBinaryCodec::Sha256File(temporary, outputSha, error)) {
        std::cerr << "Unable to hash visual index: " << error << '\n';
        return 1;
    }
    nlohmann::json optionalShards = nlohmann::json::array();
    int optionalKuroFeatureCount = 0;
    for (const auto& kuro : kuroPacks) {
        if (!kuro.loaded) continue;
        nlohmann::json shardReport;
        if (!BuildAndInstallShard(kuro.featureData, visualIndex.vocabulary,
            featureRoot / "KuroTilePacks" / kuro.directoryName / "features.yml",
            featureRoot / "KuroTilePacks" / kuro.directoryName / "visual-index.imx",
            kuro.sceneId, shardReport, error)) {
            std::cerr << "Unable to build Kuro visual shard for " << kuro.directoryName << ": " << error << '\n';
            return 1;
        }
        shardReport["packId"] = kuro.packId;
        shardReport["directory"] = kuro.directoryName;
        optionalShards.push_back(std::move(shardReport));
        optionalKuroFeatureCount += kuro.keypointCount;
    }
    int optionalCandidateFeatureCount = 0;
    for (const auto& candidate : candidates) {
        if (!candidate.loaded) continue;
        nlohmann::json shardReport;
        if (!BuildAndInstallShard(candidate.featureData, visualIndex.vocabulary,
            featureRoot / candidate.directoryName / "manifest.json",
            featureRoot / candidate.directoryName / "visual-index.imx",
            candidate.sceneId, shardReport, error)) {
            std::cerr << "Unable to build candidate visual shard for " << candidate.packId << ": " << error << '\n';
            return 1;
        }
        shardReport["packId"] = candidate.packId;
        shardReport["directory"] = candidate.directoryName;
        optionalShards.push_back(std::move(shardReport));
        optionalCandidateFeatureCount += candidate.keypointCount;
    }
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    const nlohmann::json report = {
        { "formatVersion", 1 },
        { "magic", "IMAOIX01" },
        { "sourceImf", sourceImf.filename().string() },
        { "sourceImfSha256", Hex(sourceImfSha) },
        { "visualIndexSha256", Hex(outputSha) },
        { "vocabularySha256", Hex(savedHeader.vocabularySha256) },
        { "wordCount", savedHeader.wordCount },
        { "tileSize", savedHeader.tileSize },
        { "tileStride", savedHeader.tileStride },
        { "featureCount", savedHeader.featureCount },
        { "baseFeatureCount", featureHeader.keypointCount },
        { "optionalKuroFeatureCount", optionalKuroFeatureCount },
        { "optionalCandidateFeatureCount", optionalCandidateFeatureCount },
        { "tileCount", savedHeader.tileCount },
        { "histogramEntryCount", savedHeader.histogramEntryCount },
        { "featureRowCount", savedHeader.featureRowCount },
        { "postingCount", savedHeader.postingCount },
        { "optionalShards", optionalShards },
        { "buildMilliseconds", duration }
    };
    {
        std::ofstream output(temporaryManifest, std::ios::binary | std::ios::trunc);
        output << report.dump(2) << '\n';
        if (!output.good()) {
            std::cerr << "Unable to write visual index manifest\n";
            return 1;
        }
    }
    if (!ReplaceFile(temporary, destination, error) ||
        !ReplaceFile(temporaryManifest, manifest, error)) {
        std::cerr << "Unable to install visual index: " << error << '\n';
        return 1;
    }
    std::cout << report.dump() << '\n';
    return 0;
}
