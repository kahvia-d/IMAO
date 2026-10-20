#pragma once
#include "Feature/Processing/FeatureBinaryCodec.h"
#include "App/MapViewportCandidates.h"
#include "Feature/Match/FeatureRowCache.h"
#include "Feature/Match/SceneMapFeatures.h"
#include "Feature/LayeredFloorIndex.h"
#include <filesystem>
#include <fstream>
#include "Feature/Match/AppendFeatureBatch.h"

template<class Codec>
bool LoadStoredDescriptors(const std::filesystem::path& path, ImageFeatureData& output, std::string& error) {
    if constexpr (requires { Codec::Load(path, output, error, nullptr, nullptr, true); })
        return Codec::Load(path, output, error, nullptr, nullptr, true);
    else return Codec::Load(path, output, error);
}
template<class Features>
cv::Mat ExpandStoredDescriptors(const Features& features) {
    if constexpr (requires { features.FloatDescriptors(); }) return features.FloatDescriptors();
    else return features.imgDescriptors;
}

template<class Check>
void TestDescriptorStorage(Check check) {
    const auto root = std::filesystem::temp_directory_path() / ("imao-descriptor-storage-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    ImageFeatureData input;
    input.imgDescriptors = cv::Mat(64, 128, CV_32F);
    cv::RNG random(41); random.fill(input.imgDescriptors, cv::RNG::UNIFORM, -0.5, 0.5);
    for (int row = 0; row < 64; ++row) input.imgKeypoints.emplace_back(static_cast<float>(row), 2.0f, 1.0f);
    input.imgDescriptors.row(0).copyTo(input.imgDescriptors.row(63)); input.imgKeypoints[63] = input.imgKeypoints[0];
    std::string error;
    check(FeatureBinaryCodec::Save(root / "v2.imf", input, {}, error), "storage fixture saves v2");
    ImageFeatureData baseline, stored;
    check(FeatureBinaryCodec::Load(root / "v2.imf", baseline, error) &&
        LoadStoredDescriptors<FeatureBinaryCodec>(root / "v2.imf", stored, error), "both descriptor representations load");
    check(stored.imgDescriptors.type() == CV_8UC1 &&
        stored.imgDescriptors.total() * stored.imgDescriptors.elemSize() * 4 ==
        baseline.imgDescriptors.total() * baseline.imgDescriptors.elemSize(),
        "shared descriptor storage keeps original byte codes instead of eagerly expanding fourfold");
    const auto expanded = ExpandStoredDescriptors(stored);
    check(std::memcmp(expanded.data, baseline.imgDescriptors.data, baseline.imgDescriptors.total() * sizeof(float)) == 0,
        "on-demand expansion is bit-identical to the existing codec");
    RuntimeFeatureResources normal, packed;
    normal.map = baseline; packed.map = stored;
    normal.visualIndexReady = packed.visualIndexReady = true;
    MapVisualTile tile; tile.sceneId = 1; tile.featureRowCount = 64; tile.featureRowOffset = 0;
    normal.visualIndex.tiles.push_back(tile); packed.visualIndex.tiles.push_back(tile);
    for (unsigned row = 0; row < 64; ++row) {
        normal.visualIndex.featureRows.push_back(row); packed.visualIndex.featureRows.push_back(row);
    }
    const auto expected = MapViewportCandidates::SelectSceneCandidates(normal, {0});
    const auto actual = MapViewportCandidates::SelectSceneCandidates(packed, {0});
    check(actual.imgDescriptors.type() == CV_32F && actual.imgKeypoints.size() == 63 &&
        cv::norm(actual.imgDescriptors, expected.imgDescriptors, cv::NORM_INF) == 0,
        "viewport subsets preserve float values, ordering and duplicate handling");
    const auto nearbyFeatures = SceneMapFeaturesNear(packed, 1, {0, 2}, 100);
    check(nearbyFeatures.imgDescriptors.type() == CV_32F && cv::norm(nearbyFeatures.imgDescriptors, expected.imgDescriptors, cv::NORM_INF) == 0,
        "nearby scene confirmation expands only its selected rows");
    const std::vector<std::uint32_t> rows{0, 2, 4, 63};
    FeatureRowCache plainCache(baseline), packedCache(stored);
    const auto plainRead = plainCache.Get(rows), packedRead = packedCache.Get(rows);
    check(packedRead->imgDescriptors.type() == CV_32F &&
        cv::norm(plainRead->imgDescriptors, packedRead->imgDescriptors, cv::NORM_INF) == 0,
        "minimap recovery receives the same deduplicated float subset");
    LayeredFloors::FloorEntry plainFloor, packedFloor;
    plainFloor.floorId = packedFloor.floorId = "-1/15";
    plainFloor.layerId = packedFloor.layerId = 15;
    plainFloor.features = baseline; packedFloor.features = stored;
    plainFloor.ownArt = packedFloor.ownArt = std::vector<std::uint8_t>(64, 1);
    const auto plainVote = LayeredFloors::Classify(baseline, std::vector<LayeredFloors::FloorEntry>{plainFloor});
    const auto packedVote = LayeredFloors::Classify(baseline, std::vector<LayeredFloors::FloorEntry>{packedFloor});
    check(plainVote.identified == packedVote.identified && plainVote.winnerMatches == packedVote.winnerMatches &&
        plainVote.ownDominant == packedVote.ownDominant, "full floor votes and own-art evidence are unchanged");
    check(FeatureBinaryCodec::SaveLegacyFloat32(root / "v1.imf", input, {}, error), "storage fixture saves v1");
    check(LoadStoredDescriptors<FeatureBinaryCodec>(root / "v1.imf", stored, error) && stored.imgDescriptors.type() == CV_32F,
        "legacy float packages remain readable without re-quantization");
    const auto floorRoot = root / "layered-floors";
    std::filesystem::create_directories(floorRoot);
    std::filesystem::copy_file(root / "v2.imf", floorRoot / "floor.imf");
    std::ofstream(floorRoot / "floor-index.json") <<
        R"({"formatVersion":1,"floors":[{"floorId":"-1/15","file":"floor.imf","ownMaskKeypoints":64,"ownMask":"3333333333333333"}]})";
    LayeredFloors::Index plainIndex, packedIndex;
    check(LayeredFloors::Load(root, plainIndex, error, 23) &&
        LayeredFloors::Load(root, packedIndex, error, 23, true), "floor indices load with both storage modes");
    if (!plainIndex.floors.empty() && !packedIndex.floors.empty()) {
        const auto& a = plainIndex.floors.front(); const auto& b = packedIndex.floors.front();
        check(b.features.imgDescriptors.type() == CV_8UC1 && b.features.imgDescriptors.rows == 64 &&
            b.sampleFeatures.imgDescriptors.type() == CV_32F && b.sampleFeatures.imgDescriptors.rows == 22 &&
            b.sampleFeatures.descriptorEncoding.empty() && a.ownArt == b.ownArt && a.sampleOwnArt == b.sampleOwnArt &&
            cv::norm(a.sampleFeatures.imgDescriptors, b.sampleFeatures.imgDescriptors, cv::NORM_INF) == 0,
            "packed floor Load retains full evidence and exactly aligned float samples");
        for (size_t i = 0; i < b.sampleOwnArt.size(); ++i)
            check(b.sampleOwnArt[i] == b.ownArt[i * 3], "mixed own-art mask uses the same sampling stride");
        for (bool samples : {false, true}) {
            const auto av = LayeredFloors::Classify(baseline, plainIndex.floors, 10, 2, .75f, .6f, samples);
            const auto bv = LayeredFloors::Classify(baseline, packedIndex.floors, 10, 2, .75f, .6f, samples);
            check(av.identified == bv.identified && av.winnerMatches == bv.winnerMatches &&
                av.ownDominant == bv.ownDominant, "loaded full/sample own-art votes agree with legacy float storage");
        }
    }
    // Two v2 packs may have different ranges. The boundary must decode using
    // the source pack's scale, including after a legacy float pack is appended.
    ImageFeatureData secondInput = input;
    secondInput.imgDescriptors = input.imgDescriptors * 3.75f;
    check(FeatureBinaryCodec::Save(root / "other.imf", secondInput, {}, error), "second scale fixture saves");
    ImageFeatureData firstPacked, secondPacked, secondFloat;
    LoadStoredDescriptors<FeatureBinaryCodec>(root / "v2.imf", firstPacked, error);
    LoadStoredDescriptors<FeatureBinaryCodec>(root / "other.imf", secondPacked, error);
    FeatureBinaryCodec::Load(root / "other.imf", secondFloat, error);
    ImageFeatureData merged;
    check(AppendFeatureBatch(merged, {&firstPacked, &secondPacked}, error), "multiple encoded packs merge");
    auto mergedFloat = merged.FloatDescriptors();
    check(merged.imgDescriptors.type() == CV_8UC1 && merged.imgDescriptors.rows == 128 &&
        std::memcmp(mergedFloat.ptr(0), baseline.imgDescriptors.data, baseline.imgDescriptors.total() * sizeof(float)) == 0 &&
        std::memcmp(mergedFloat.ptr(64), secondFloat.imgDescriptors.data, secondFloat.imgDescriptors.total() * sizeof(float)) == 0,
        "merged row ranges retain each pack scale and exact descriptor values");
    check(AppendFeatureBatch(merged, {&baseline}, error) && merged.imgDescriptors.type() == CV_32FC1 &&
        merged.imgDescriptors.rows == 192 && merged.descriptorEncoding.empty() &&
        cv::norm(merged.imgDescriptors.rowRange(0,128), mergedFloat, cv::NORM_INF) == 0,
        "mixed legacy/encoded batches expand without re-quantization or row shifts");
    bool cancelled = false;
    try { UniqueFeatureSubset(firstPacked, rows, [] { return true; }); } catch (const SearchInterrupted&) { cancelled = true; }
    check(cancelled, "candidate materialization obeys cancellation");
    {
        std::fstream file(root / "v2.imf", std::ios::binary | std::ios::in | std::ios::out);
        file.seekg(-1, std::ios::end); char byte = 0; file.read(&byte,1);
        byte ^= 0x40; file.seekp(-1, std::ios::end); file.write(&byte,1);
    }
    check(!LoadStoredDescriptors<FeatureBinaryCodec>(root / "v2.imf", stored, error) &&
        stored.imgDescriptors.empty() && stored.descriptorEncoding.empty() && !error.empty(),
        "encoded storage still rejects corruption and clears partial output");
    std::filesystem::remove_all(root);
}

// Optional local-resource comparison: opens one file pair at a time, performs no
// writes to resource packs, and verifies every float bit and each selected subset.
inline int CompareDescriptorStorage(const std::filesystem::path& root) {
    uint64_t files = 0, floatBytes = 0, storedBytes = 0;
    std::string error;
    for (const auto& item : std::filesystem::recursive_directory_iterator(root)) {
        if (!item.is_regular_file() || item.path().extension() != ".imf") continue;
        ImageFeatureData normal, packed;
        if (!FeatureBinaryCodec::Load(item.path(), normal, error) ||
            !LoadStoredDescriptors<FeatureBinaryCodec>(item.path(), packed, error)) {
            std::cerr << "descriptor comparison load failed: " << error << '\n'; return 1;
        }
        const auto expanded = packed.FloatDescriptors();
        const auto bytes = normal.imgDescriptors.total() * sizeof(float);
        if (std::memcmp(normal.imgDescriptors.data, expanded.data, bytes) != 0) {
            std::cerr << "descriptor bit mismatch: " << item.path() << '\n'; return 1;
        }
        std::vector<uint32_t> rows;
        for (int row = 0; row < normal.imgDescriptors.rows; row += (std::max)(1, normal.imgDescriptors.rows / 64))
            rows.push_back(static_cast<uint32_t>(row));
        const auto before = UniqueFeatureSubset(normal, rows), after = UniqueFeatureSubset(packed, rows);
        if (before.imgKeypoints.size() != after.imgKeypoints.size() ||
            cv::norm(before.imgDescriptors, after.imgDescriptors, cv::NORM_INF) != 0) {
            std::cerr << "candidate subset mismatch: " << item.path() << '\n'; return 1;
        }
        ++files; floatBytes += bytes; storedBytes += packed.imgDescriptors.total() * packed.imgDescriptors.elemSize();
    }
    std::cout << "Descriptor comparison files=" << files << " floatBytes=" << floatBytes <<
        " storedBytes=" << storedBytes << " savedBytes=" << (floatBytes - storedBytes) << '\n';
    return files ? 0 : 1;
}
