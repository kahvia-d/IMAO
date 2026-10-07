// Verifies that a layout's registered tile packs load and that their visual shards
// merge into one vocabulary, without a base world pack present.
//
// This mirrors RuntimeFeatureRepository's pack loop line for line (snapshot mode
// included) and reports per-pack load/vocabulary state, so "all packs load" is a
// measurement rather than an assumption. The whole-tree regression tool cannot do
// this job: it requires a base world pack, which this project retired.
#include "Feature/KuroTileFeaturePack.h"
#include "Feature/VisualIndex/MapVisualIndex.h"
#include "Runtime/ResourceSnapshotContext.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace std::filesystem;

/// The same merge the runtime performs, reduced to what this probe checks: that the
/// shard's vocabulary matches the one the first shard established.
bool MergeVocabulary(const MapVisualIndex& shard, cv::Mat& vocabulary,
    std::array<std::uint8_t, 32>& vocabularySha256, const std::string& pack,
    std::size_t& tileCount, std::string& error) {
    if (vocabulary.empty()) {
        vocabulary = shard.vocabulary.clone();
        vocabularySha256 = shard.vocabularySha256;
    }
    if (vocabularySha256 != shard.vocabularySha256 || vocabulary.size() != shard.vocabulary.size()) {
        error = pack + ": shard vocabulary does not match the first shard";
        return false;
    }
    tileCount += shard.tiles.size();
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: IMaoPackMergeProbe <Assets directory>\n";
        return 2;
    }
    const path assetRoot = absolute(argv[1]);
    const path featureRoot = assetRoot / "FeaturesDatas";
    if (!is_directory(featureRoot)) {
        std::cerr << "no FeaturesDatas under " << assetRoot << '\n';
        return 2;
    }

    // Point the resource snapshot context at this tree if it carries one, exactly as
    // the host does when it starts in snapshot mode.
    if (exists(assetRoot / "Updates" / "bundled-snapshot.json")) {
        SetCurrentDirectoryW(assetRoot.c_str());
    }

    const auto packs = KuroTileFeaturePack::LoadRegistered(featureRoot.string());
    std::cout << "registered packs: " << packs.size() << '\n';

    const auto approved = std::count_if(packs.begin(), packs.end(),
        [](const auto& pack) { return pack.loaded && pack.runtimeApproved; });
    std::cout << "loaded and runtime-approved: " << approved << '\n';

    cv::Mat vocabulary;
    std::array<std::uint8_t, 32> vocabularySha256{};
    std::size_t mergedTiles = 0;
    std::vector<std::string> failures;
    std::uint64_t totalKeypoints = 0;

    for (const auto& pack : packs) {
        if (!pack.loaded || !pack.runtimeApproved) {
            failures.push_back(pack.directoryName + ": not loaded (" + pack.error + ")");
            std::cout << "  " << pack.directoryName << ": NOT LOADED  error=" << pack.error << '\n';
            continue;
        }
        totalKeypoints += pack.featureData.imgKeypoints.size();
        MapVisualIndex shard;
        std::string error;
        const bool ready = MapVisualIndexCodec::Load(pack.directoryPath / "visual-index.imx",
            pack.sourceSha256, static_cast<std::uint32_t>(pack.featureData.imgKeypoints.size()),
            shard, error);
        std::string mergeError;
        const bool merged = ready && MergeVocabulary(shard, vocabulary, vocabularySha256,
            pack.directoryName, mergedTiles, mergeError);
        if (!merged) failures.push_back(pack.directoryName + ": " + (ready ? mergeError : error));
        std::cout << "  " << pack.directoryName << ": keypoints=" << pack.featureData.imgKeypoints.size()
                  << " shardTiles=" << (ready ? shard.tiles.size() : 0)
                  << " shard=" << (ready ? "read" : "FAILED")
                  << " merge=" << (merged ? "ok" : "FAILED")
                  << (merged ? "" : "  " + (ready ? mergeError : error)) << '\n';
    }

    std::cout << '\n';
    std::cout << "total keypoints : " << totalKeypoints << '\n';
    std::cout << "merged tiles    : " << mergedTiles << '\n';
    std::cout << "vocabulary set  : " << (vocabulary.empty() ? "no" : "yes") << '\n';
    std::cout << "failures        : " << failures.size() << '\n';
    for (const auto& failure : failures) std::cout << "   " << failure << '\n';
    return failures.empty() ? 0 : 1;
}
