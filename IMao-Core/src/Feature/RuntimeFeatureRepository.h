#pragma once

#include "Match/FeatureMatch.h"
#include "VisualIndex/MapVisualIndex.h"

#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct VisualFeatureShardRange {
    int sceneId = 0;
    std::uint32_t firstTile = 0;
    std::uint32_t tileCount = 0;
};

struct RuntimeFeatureResources {
    ImageFeatureData map;
    MapVisualIndex visualIndex;
    // Legacy atlas retrieval partitions are not authoritative scene metadata.
    // Loaded replacement packs may retire hash-bound duplicate feature rows.
    std::uint32_t baseVisualTileCount = 0;
    std::vector<unsigned char> excludedBaseRows;
    bool FeatureRowEnabled(std::size_t row) const {
        return row >= excludedBaseRows.size() || excludedBaseRows[row] == 0;
    }
    // Optional Kuro feature packs are kept separate so an image-only fallback
    // can verify one map package at a time without cross-region false matches.
    // The separation is expressed by row ranges, not by a second copy of the
    // descriptors: kuroVisualShards says which rows of the merged map belong to
    // which pack, and the feature rows themselves are read through the index.
    std::vector<VisualFeatureShardRange> kuroVisualShards;
    bool visualIndexReady = false;
    // There used to be an `ImageFeatureData kuroTileFeatures` here. Every tile pack was appended to it
    // after being merged into `map` above, so it held a second full copy of the same descriptors -
    // ~630 MB resident at 1.18 M keypoints - and nothing had read it since the per-region path moved to
    // kuroVisualShards. Removed 2026-10-02 after the memory audit measured the copy; a future consumer
    // that needs one region's rows should slice `map` with the shard range instead of materialising them.
    ImageFeatureData curatedCandidates;
    ImageFeatureData iconTask;
    ImageFeatureData wavePlateCrystal;
};

class RuntimeFeatureRepository {
public:
    static RuntimeFeatureRepository& Instance();

    void BeginPreload(const std::filesystem::path& assetRoot);
    std::shared_ptr<const RuntimeFeatureResources> AwaitReady(std::string& error);
    bool IsReady() const;
    void Shutdown();

    RuntimeFeatureRepository(const RuntimeFeatureRepository&) = delete;
    RuntimeFeatureRepository& operator=(const RuntimeFeatureRepository&) = delete;

private:
    RuntimeFeatureRepository() = default;
    ~RuntimeFeatureRepository();
    void Load(std::stop_token stopToken, std::filesystem::path assetRoot);

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::jthread preloadThread_;
    bool started_ = false;
    bool completed_ = false;
    std::shared_ptr<const RuntimeFeatureResources> resources_;
    std::string error_;
};
