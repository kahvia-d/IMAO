/// Strips the shared vocabulary out of a region pack's visual index, and proves it is safe.
///
/// Every region pack carries its own copy of the same 4096x128 float vocabulary: 28.00 MB
/// across 14 packs, of a matrix zlib cannot meaningfully shrink. The runtime can take that
/// vocabulary from one program-level file instead, but only if removing it from a shard
/// changes nothing that the merge, the FLANN vocabulary index, or the probes can observe.
///
/// This tool does not trust that. For every pack it strips the vocabulary into a temporary
/// file, loads that file back against the vocabulary, re-serialises the result, and requires
/// the bytes to equal the original shard exactly. Anything less than byte equality is a
/// failure, not a warning.
///
/// Verification is the default. --apply rewrites the pack's shard and updates the recorded
/// hash in visual-index.manifest.json, after the same check has passed.
///
/// Usage: IMaoVisualIndexRepack <pack directory> [--apply --vocabulary <path>]

#include "Feature/Processing/FeatureBinaryCodec.h"
#include "Feature/VisualIndex/MapVisualIndex.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> ReadAll(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return {};
    const auto size = input.tellg();
    input.seekg(0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

bool WriteAll(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    return output.good();
}

std::string Megabytes(std::uint64_t bytes) {
    char text[32] = {};
    std::snprintf(text, sizeof(text), "%.2f", static_cast<double>(bytes) / 1048576.0);
    return text;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: IMaoVisualIndexRepack <pack directory> [--apply --vocabulary <path>]\n";
        return 2;
    }
    const std::filesystem::path packRoot(argv[1]);
    bool apply = false;
    std::filesystem::path vocabularyPath;
    for (int index = 2; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--apply") {
            apply = true;
        }
        else if (argument == "--vocabulary" && index + 1 < argc) {
            vocabularyPath = argv[++index];
        }
        else {
            std::cerr << "Unknown argument: " << argument << '\n';
            return 2;
        }
    }
    if (apply && vocabularyPath.empty()) {
        std::cerr << "--apply needs --vocabulary <path> so the shared vocabulary has a home.\n";
        return 2;
    }

    const auto shardPath = packRoot / "visual-index.imx";
    const auto manifestPath = packRoot / "visual-index.manifest.json";
    std::string error;

    ImageFeatureData features;
    FeatureBinaryHeader featureHeader;
    if (!FeatureBinaryCodec::Load(packRoot / "features.imf", features, error, &featureHeader, nullptr)) {
        std::cerr << packRoot.filename().string() << ": features.imf: " << error << '\n';
        return 1;
    }

    MapVisualIndex original;
    if (!MapVisualIndexCodec::Load(shardPath, featureHeader.sourceXmlSha256,
            featureHeader.keypointCount, original, error)) {
        std::cerr << packRoot.filename().string() << ": shard rejected: " << error << '\n';
        return 1;
    }
    const auto originalBytes = ReadAll(shardPath);
    if (originalBytes.empty()) {
        std::cerr << packRoot.filename().string() << ": shard is unreadable\n";
        return 1;
    }

    const auto scratch = std::filesystem::temp_directory_path() /
        ("imao-repack-" + packRoot.filename().string());
    std::filesystem::create_directories(scratch);
    const auto strippedPath = scratch / "stripped.imx";
    const auto restoredPath = scratch / "restored.imx";
    const auto scratchVocabulary = scratch / "vocabulary.imx";

    // The vocabulary travels with the shard that was stripped, so a fresh build can hand the
    // rebuilt index's vocabulary to the same check.
    if (!MapVisualIndexCodec::SaveVocabulary(scratchVocabulary, original.vocabulary, error)) {
        std::cerr << packRoot.filename().string() << ": " << error << '\n';
        return 1;
    }
    if (!MapVisualIndexCodec::Save(strippedPath, original, error, nullptr, false)) {
        std::cerr << packRoot.filename().string() << ": " << error << '\n';
        return 1;
    }

    cv::Mat vocabulary;
    std::array<std::uint8_t, 32> vocabularySha256{};
    if (!MapVisualIndexCodec::LoadVocabulary(scratchVocabulary, vocabulary, vocabularySha256, error)) {
        std::cerr << packRoot.filename().string() << ": " << error << '\n';
        return 1;
    }

    MapVisualIndex reloaded;
    if (!MapVisualIndexCodec::Load(strippedPath, featureHeader.sourceXmlSha256, featureHeader.keypointCount,
            reloaded, error, nullptr, &original)) {
        std::cerr << packRoot.filename().string() << ": stripped shard does not load back: " << error << '\n';
        return 1;
    }
    if (!MapVisualIndexCodec::Save(restoredPath, reloaded, error)) {
        std::cerr << packRoot.filename().string() << ": " << error << '\n';
        return 1;
    }

    const auto strippedBytes = ReadAll(strippedPath);
    const auto restoredBytes = ReadAll(restoredPath);
    const bool identical = restoredBytes == originalBytes;
    const std::uint64_t saved = originalBytes.size() - strippedBytes.size();

    std::cout << packRoot.filename().string()
              << "  shard " << Megabytes(originalBytes.size()) << " MB -> "
              << Megabytes(strippedBytes.size()) << " MB  (saved " << Megabytes(saved) << " MB, "
              << original.tiles.size() << " tiles, " << original.featureCount << " features)  "
              << (identical ? "IDENTICAL" : "*** MISMATCH ***") << '\n';
    if (!identical) {
        std::cerr << packRoot.filename().string()
                  << ": reloading the stripped shard did not reproduce the original index.\n";
        return 1;
    }

    if (!apply) {
        std::filesystem::remove_all(scratch);
        return 0;
    }

    // Re-check the shared vocabulary every pack contributes, so a sweep cannot quietly mix two.
    if (std::filesystem::exists(vocabularyPath)) {
        cv::Mat existing;
        std::array<std::uint8_t, 32> existingSha256{};
        if (!MapVisualIndexCodec::LoadVocabulary(vocabularyPath, existing, existingSha256, error)) {
            std::cerr << "shared vocabulary is unusable: " << error << '\n';
            return 1;
        }
        if (existingSha256 != vocabularySha256) {
            std::cerr << "shared vocabulary does not match " << packRoot.filename().string()
                      << " - refusing to mix two vocabularies in one build.\n";
            return 1;
        }
    }
    else if (!MapVisualIndexCodec::SaveVocabulary(vocabularyPath, vocabulary, error)) {
        std::cerr << "unable to write the shared vocabulary: " << error << '\n';
        return 1;
    }

    if (!WriteAll(shardPath, strippedBytes)) {
        std::cerr << packRoot.filename().string() << ": unable to rewrite the shard\n";
        return 1;
    }

    // visual-index.manifest.json records the shard's hash; leaving it stale would make the
    // pack fail its own checks even though the index itself is fine.
    if (std::filesystem::exists(manifestPath)) {
        std::ifstream input(manifestPath);
        auto manifest = nlohmann::json::parse(input, nullptr, false);
        if (manifest.is_discarded() || !manifest.is_object()) {
            std::cerr << packRoot.filename().string() << ": manifest is not valid JSON\n";
            return 1;
        }
        std::array<std::uint8_t, 32> shardHash{};
        if (!FeatureBinaryCodec::Sha256File(shardPath, shardHash, error)) {
            std::cerr << packRoot.filename().string() << ": " << error << '\n';
            return 1;
        }
        manifest["visualIndexSha256"] = FeatureBinaryCodec::Sha256Hex(shardHash);
        std::ofstream output(manifestPath, std::ios::binary | std::ios::trunc);
        output << manifest.dump(2) << '\n';
        output.flush();
        if (!output.good()) {
            std::cerr << packRoot.filename().string() << ": unable to rewrite the manifest\n";
            return 1;
        }
    }

    std::filesystem::remove_all(scratch);
    std::cout << packRoot.filename().string() << "  rewritten\n";
    return 0;
}
