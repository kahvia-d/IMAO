#include "Feature/CandidateFeaturePack.h"
#include "Feature/KuroTileFeaturePack.h"
#include "Feature/Match/FeatureMatch.h"
#include "Feature/Processing/FeatureBinaryCodec.h"
#include "Feature/VisualIndex/MapVisualIndex.h"

#include <nlohmann/json.hpp>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cctype>
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
    const std::array<std::uint8_t, 32>& sourceHash, const std::string& sourceName,
    const std::filesystem::path& destination,
    int sceneId, nlohmann::json& report, std::string& error) {
    MapVisualIndex shard;
    if (!MapVisualIndexBuilder::Build(features.imgKeypoints, features.imgDescriptors,
        sourceHash, shard, error, vocabulary, sceneId)) return false;
    std::filesystem::create_directories(destination.parent_path());
    const auto temporary = destination.wstring() + L".tmp";
    MapVisualIndexHeader savedHeader;
    // The shard is written without the vocabulary: the 4096x128 matrix is the same 2.00 MB in
    // every region pack, so it ships once as a program-level file and each shard names it by
    // hash. A rebuilt pack that quietly kept its own copy would give the size win back.
    if (!MapVisualIndexCodec::Save(temporary, shard, error, &savedHeader, false)) return false;
    MapVisualIndex verified;
    // The round trip therefore has to supply the vocabulary the shard was written against,
    // which is exactly what the runtime does with the shared file.
    if (!MapVisualIndexCodec::Load(temporary, sourceHash, shard.featureCount, verified, error,
            nullptr, &shard) || !Equivalent(shard, verified)) {
        if (error.empty()) error = "optional visual shard round-trip mismatch";
        return false;
    }
    std::array<std::uint8_t, 32> outputHash{};
    if (!FeatureBinaryCodec::Sha256File(temporary, outputHash, error) ||
        !ReplaceFile(temporary, destination, error)) return false;
    report = {
        {"path", destination.filename().string()}, {"sceneId", sceneId},
        {"source", sourceName}, {"sourceSha256", Hex(sourceHash)},
        {"visualIndexSha256", Hex(outputHash)}, {"vocabularySha256", Hex(savedHeader.vocabularySha256)},
        {"featureCount", savedHeader.featureCount}, {"tileCount", savedHeader.tileCount},
        {"postingCount", savedHeader.postingCount}
    };
    return true;
}

/// Reads just the vocabulary out of an existing index, or out of the shared vocabulary file.
///
/// Deliberately not MapVisualIndexCodec::Load: that validates the whole index against the
/// source it was built from, and a rebuild wants the words, not the identity of whatever
/// laid them out. The vocabulary carries its own hash in the header, which is checked here,
/// so the words and their identity still come from one verified source.
///
/// The shared vocabulary file is tried first because it is now the source that is actually
/// available. A region shard cannot be used any more: shards no longer carry the vocabulary,
/// they name it by hash. A build that still points at a shard gets told so below.
bool ReadVocabulary(const std::filesystem::path& index, const std::filesystem::path& vocabularySource,
    cv::Mat& vocabulary, std::array<std::uint8_t, 32>& vocabularySha256, std::string& error) {
    {
        cv::Mat shared;
        std::array<std::uint8_t, 32> sharedSha256{};
        std::string sharedError;
        if (MapVisualIndexCodec::LoadVocabulary(vocabularySource, shared, sharedSha256, sharedError)) {
            vocabulary = shared;
            vocabularySha256 = sharedSha256;
            error.clear();
            return true;
        }
    }
    std::ifstream input(vocabularySource, std::ios::binary);
    if (!input) {
        error = "vocabulary source cannot be opened: " + vocabularySource.string();
        return false;
    }
    std::vector<std::uint8_t> header(MapVisualIndexHeader::SerializedSize);
    input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (input.gcount() != static_cast<std::streamsize>(header.size()) ||
        !std::equal(MapVisualIndexHeader::Magic.begin(), MapVisualIndexHeader::Magic.end(), header.begin())) {
        error = "vocabulary source is not an IMAOIX01 index";
        return false;
    }
    const auto read32 = [&header](std::size_t offset) {
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            value |= static_cast<std::uint32_t>(header[offset + index]) << (index * 8);
        }
        return value;
    };
    const auto read64 = [&header](std::size_t offset) {
        std::uint64_t value = 0;
        for (std::size_t index = 0; index < 8; ++index) {
            value |= static_cast<std::uint64_t>(header[offset + index]) << (index * 8);
        }
        return value;
    };
    const auto version = read32(8);
    const auto wordCount = read32(20);
    const auto descriptorColumns = read32(24);
    const auto vocabularyPayloadLength = read64(8 + 12 * 4);
    if (version != MapVisualIndexHeader::CurrentVersion ||
        wordCount != MapVisualIndex::WordCount || descriptorColumns != MapVisualIndex::DescriptorColumns ||
        vocabularyPayloadLength != static_cast<std::uint64_t>(wordCount) * descriptorColumns * sizeof(float)) {
        // A shard built after the vocabulary moved out of the shards has a zero vocabulary
        // payload, and "does not describe a vocabulary" would leave the reader guessing.
        error = vocabularyPayloadLength == 0
            ? "vocabulary source is a shard that no longer carries a vocabulary; pass the shared "
              "Map_visual_vocabulary.imx instead: " + vocabularySource.string()
            : "vocabulary source header does not describe a " +
                std::to_string(MapVisualIndex::WordCount) + "x" +
                std::to_string(MapVisualIndex::DescriptorColumns) + " vocabulary";
        return false;
    }
    std::vector<std::uint8_t> payload(static_cast<std::size_t>(vocabularyPayloadLength));
    input.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    if (input.gcount() != static_cast<std::streamsize>(payload.size())) {
        error = "vocabulary payload is truncated";
        return false;
    }
    vocabulary = cv::Mat(MapVisualIndex::WordCount, MapVisualIndex::DescriptorColumns, CV_32FC1, payload.data()).clone();
    std::copy_n(header.begin() + 8 + 12 * 4 + 5 * 8 + 32, vocabularySha256.size(), vocabularySha256.begin());
    error.clear();
    static_cast<void>(index);
    return true;
}

/// The hash a shard records as its source. A pack's own feature source (features.yml)
/// is a build-time input that is deliberately not shipped, so the hash comes from the
/// binary header - which is the same value the binary was converted with, and the one
/// Stage-UpdateResources checks the pack manifest against. Reading the file when it
/// happens to be present would be a second, weaker source for the same fact.
bool ShardSourceHash(const ImageFeatureData& features, const FeatureBinaryHeader& header,
    const nlohmann::json& manifest, std::array<std::uint8_t, 32>& sourceHash,
    std::string& sourceName, std::string& error) {
    sourceHash = header.sourceXmlSha256;
    sourceName = manifest.contains("features") && manifest.at("features").contains("file")
        ? manifest.at("features").at("file").get<std::string>() : std::string("(unknown source)");
    if (manifest.contains("features")) {
        auto recorded = manifest.at("features").value("sha256", std::string());
        std::transform(recorded.begin(), recorded.end(), recorded.begin(),
            [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        if (!recorded.empty() && FeatureBinaryCodec::Sha256Hex(sourceHash) != recorded) {
            error = "pack manifest records a different feature source hash than its binary";
            return false;
        }
        const auto count = manifest.at("features").value("keypointCount", 0);
        if (count != 0 && count != static_cast<int>(features.imgKeypoints.size())) {
            error = "pack manifest keypoint count does not match its binary";
            return false;
        }
    }
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
            std::array<std::uint8_t, 32> baseHash{}, sourceHash{};
            std::string sourceName;
            MapVisualIndex baseline;
            if (!FeatureBinaryCodec::Load(featureRoot / "Map_features.imf", base, error, &baseHeader, &baseHash) ||
                !MapVisualIndexCodec::Load(featureRoot / "Map_visual_index.imx", baseHash, baseHeader.keypointCount, baseline, error) ||
                !FeatureBinaryCodec::Load(packRoot / "features.imf", features, error, &packHeader)) throw std::runtime_error(error);
            if (!ShardSourceHash(features, packHeader, manifest, sourceHash, sourceName, error))
                throw std::runtime_error(error);
            // The shard is written without the vocabulary and names it by hash, so the file it
            // names has to exist next to the packs and has to be the one this build used. Writing
            // it here is what lets a brand-new region pack be built on a tree that has never run a
            // full index build; comparing it is what stops a new pack from being built against a
            // vocabulary the shipped packs do not share, which the runtime drops without saying so.
            const auto sharedVocabularyPath = featureRoot / "Map_visual_vocabulary.imx";
            cv::Mat existingVocabulary;
            std::array<std::uint8_t, 32> existingSha256{};
            std::string vocabularyError;
            if (std::filesystem::exists(sharedVocabularyPath)) {
                if (!MapVisualIndexCodec::LoadVocabulary(sharedVocabularyPath, existingVocabulary,
                        existingSha256, vocabularyError) || existingSha256 != baseline.vocabularySha256) {
                    throw std::runtime_error("shared vocabulary does not match the baseline this pack "
                        "was built against: " + vocabularyError);
                }
            }
            else if (!MapVisualIndexCodec::SaveVocabulary(sharedVocabularyPath, baseline.vocabulary,
                    vocabularyError)) {
                throw std::runtime_error("unable to write the shared vocabulary: " + vocabularyError);
            }
            nlohmann::json report;
            if (!BuildAndInstallShard(features, baseline.vocabulary, sourceHash, sourceName,
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
    if (argc < 3 || argc > 6) {
        std::cerr << "Usage: IMaoVisualIndexBuilder <Assets directory> <output.imx> [manifest.json]\n"
                     "                                   [--base-only | --reuse-vocabulary <existing.imx>]\n"
                     "  --base-only          rebuilds just the base index and leaves the packs' shards alone.\n"
                     "  --reuse-vocabulary   rebuilds the base index AND every pack shard against the\n"
                     "                       vocabulary an existing index already carries, instead of\n"
                     "                       clustering a new one. Every index that has to merge must share\n"
                     "                       one vocabulary, so a partial rebuild is not an option.\n"
                     "  Always writes FeaturesDatas/Map_visual_vocabulary.imx: the region shards are\n"
                     "  written without the vocabulary and name that file by hash.\n";
        return 2;
    }
    // The packs' shards are rebuilt from each pack's own feature source, which is not
    // always present (feature binaries are shipped without the XML they came from).
    // --base-only exists so the base index can still be rebuilt in that case, and
    // --reuse-vocabulary so a full rebuild can keep the vocabulary the shipped shards
    // were built against.
    const bool baseOnly = argc == 5 && std::string(argv[4]) == "--base-only";
    if (argc == 5 && !baseOnly) {
        std::cerr << "Unknown option: " << argv[4] << '\n';
        return 2;
    }
    const bool reuseVocabulary = argc == 6 && std::string(argv[4]) == "--reuse-vocabulary";
    if (argc == 6 && !reuseVocabulary) {
        std::cerr << "Unknown option: " << argv[4] << '\n';
        return 2;
    }
    const std::filesystem::path assetRoot = std::filesystem::absolute(argv[1]);
    const std::filesystem::path featureRoot = assetRoot / "FeaturesDatas";
    const std::filesystem::path sourceImf = featureRoot / "Map_features.imf";
    const std::filesystem::path destination = std::filesystem::absolute(argv[2]);
    // The caller always names the manifest when it passes a switch, so the position of
    // the manifest is the same in every form that carries one.
    const bool manifestGiven = argc == 4 || argc == 6;
    const std::filesystem::path manifest = manifestGiven
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

    // Reusing a vocabulary is what makes a full rebuild safe to ship: every index that
    // has to merge must name the same words, and clustering a fresh vocabulary from
    // re-encoded descriptors produces a different one (the seed only makes the run
    // repeatable, it does not make it insensitive to the input). The vocabulary is read
    // straight out of an existing index, so the words and their identity are copied.
    cv::Mat reusedVocabulary;
    if (reuseVocabulary) {
        const std::filesystem::path reference = std::filesystem::absolute(argv[5]);
        std::array<std::uint8_t, 32> reusedVocabularySha{};
        if (!ReadVocabulary(destination, reference, reusedVocabulary, reusedVocabularySha, error)) {
            std::cerr << "Unable to read the vocabulary to reuse: " << error << '\n';
            return 1;
        }
        std::cout << "reusing vocabulary " << Hex(reusedVocabularySha) << " from "
                  << reference.filename().string() << '\n';
    }

    const auto start = std::chrono::steady_clock::now();
    MapVisualIndex visualIndex;
    // Preserve the legacy nearest-origin partitioning here: it is part of the
    // retrieval index and changing it changes ranking.  At runtime, successful
    // base-index matches are reported as World (see GlobalVisualLocalizer).
    if (!MapVisualIndexBuilder::Build(features.imgKeypoints, features.imgDescriptors,
            sourceImfSha, visualIndex, error, reusedVocabulary)) {
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

    // Every region shard below is written without the vocabulary, so the build has to emit the
    // one those shards name by hash. It goes next to them, which is where the runtime reads it
    // from, and writing it here rather than behind a flag means a rebuild cannot forget it and
    // quietly produce shards that no runtime can read.
    if (!MapVisualIndexCodec::SaveVocabulary(featureRoot / "Map_visual_vocabulary.imx",
            visualIndex.vocabulary, error)) {
        std::cerr << "Unable to write the shared vocabulary: " << error << '\n';
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
        if (!kuro.loaded || baseOnly) continue;
        // The pack status already carries the hash its manifest records for the feature
        // source, and the loader refuses a pack whose binary disagrees with it, so the
        // shard can be built from the binary without the source file being present.
        std::string sourceName = std::string("KuroTilePacks/") + kuro.directoryName + "/features.yml";
        nlohmann::json shardReport;
        if (!BuildAndInstallShard(kuro.featureData, visualIndex.vocabulary, kuro.sourceSha256, sourceName,
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
        if (!candidate.loaded || baseOnly) continue;
        // Candidates ship their own manifest and keep hashing it: the status carries no
        // feature-source hash, and no candidate pack is published today.
        const auto candidateManifest = featureRoot / candidate.directoryName / "manifest.json";
        std::array<std::uint8_t, 32> candidateSourceHash{};
        if (!FeatureBinaryCodec::Sha256File(candidateManifest, candidateSourceHash, error)) {
            std::cerr << "Unable to hash candidate manifest for " << candidate.directoryName << ": " << error << '\n';
            return 1;
        }
        std::string sourceName = candidate.directoryName + "/manifest.json";
        nlohmann::json shardReport;
        if (!BuildAndInstallShard(candidate.featureData, visualIndex.vocabulary,
            candidateSourceHash, sourceName,
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
