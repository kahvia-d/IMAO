#include "RuntimeFeatureRepository.h"

#include "CandidateFeaturePack.h"
#include "KuroTileFeaturePack.h"
#include "Processing/FeatureBinaryCodec.h"
#include "Processing/FeatureProcessing.h"
#include "VisualIndex/MapVisualIndex.h"
#include "../Diagnostics/Diagnostics.h"

#include <algorithm>
#include <chrono>
#include <limits>

namespace {
long long ElapsedMilliseconds(const std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
}

bool MergeVisualShard(MapVisualIndex& base, const MapVisualIndex& shard,
    std::uint32_t featureRowBase, std::string& error) {
    if (base.vocabularySha256 != shard.vocabularySha256 ||
        base.vocabulary.size() != shard.vocabulary.size()) {
        error = "optional visual shard vocabulary does not match the base index";
        return false;
    }
    const auto tileBase = static_cast<std::uint32_t>(base.tiles.size());
    const auto histogramBase = static_cast<std::uint32_t>(base.histograms.size());
    const auto storedRowBase = static_cast<std::uint32_t>(base.featureRows.size());
    if (static_cast<std::uint64_t>(featureRowBase) + shard.featureCount >
        std::numeric_limits<std::uint32_t>::max()) {
        error = "optional visual shard feature row offset overflow";
        return false;
    }
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
}

RuntimeFeatureRepository& RuntimeFeatureRepository::Instance() {
    static RuntimeFeatureRepository repository;
    return repository;
}

RuntimeFeatureRepository::~RuntimeFeatureRepository() {
    Shutdown();
}

void RuntimeFeatureRepository::BeginPreload(const std::filesystem::path& assetRoot) {
    std::scoped_lock lock(mutex_);
    if (started_) return;
    started_ = true;
    completed_ = false;
    error_.clear();
    preloadThread_ = std::jthread([this, assetRoot](std::stop_token stopToken) {
        Load(stopToken, assetRoot);
    });
}

std::shared_ptr<const RuntimeFeatureResources> RuntimeFeatureRepository::AwaitReady(std::string& error) {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] { return completed_; });
    error = error_;
    return resources_;
}

bool RuntimeFeatureRepository::IsReady() const {
    std::scoped_lock lock(mutex_);
    return completed_ && resources_ != nullptr;
}

void RuntimeFeatureRepository::Shutdown() {
    std::jthread thread;
    {
        std::scoped_lock lock(mutex_);
        if (preloadThread_.joinable()) preloadThread_.request_stop();
        thread = std::move(preloadThread_);
    }
    if (thread.joinable()) thread.join();
    {
        std::scoped_lock lock(mutex_);
        resources_.reset();
        error_.clear();
        started_ = false;
        completed_ = false;
    }
}

void RuntimeFeatureRepository::Load(std::stop_token stopToken, std::filesystem::path assetRoot) {
    const auto totalStart = std::chrono::steady_clock::now();
    auto loaded = std::make_shared<RuntimeFeatureResources>();
    std::string failure;
    try {
        const auto featureRoot = assetRoot / "FeaturesDatas";
        const auto mapStart = std::chrono::steady_clock::now();
        std::array<std::uint8_t, 32> sourceImfSha{};
        bool sourceImfHashReady = FeatureBinaryCodec::Load(
            featureRoot / "Map_features.imf", loaded->map, failure, nullptr, &sourceImfSha);
        if (!sourceImfHashReady) {
#ifdef IMAO_ALLOW_XML_FEATURE_FALLBACK
            Diagnostics::Record("resource-load", "stage=map-imf failed fallback=xml error=" + failure);
            if (!FeatureLoader::loadFeaturesFromXML((featureRoot / "Map_features.yml").string(), loaded->map)) {
                throw std::runtime_error("map IMF and XML fallback both failed: " + failure);
            }
#else
            throw std::runtime_error(failure);
#endif
        }
        Diagnostics::Record("resource-load", "stage=map-features durationMs=" +
            std::to_string(ElapsedMilliseconds(mapStart)) + " keypoints=" +
            std::to_string(loaded->map.imgKeypoints.size()));
        if (stopToken.stop_requested()) throw std::runtime_error("feature preload cancelled");

        const auto visualStart = std::chrono::steady_clock::now();
        std::string visualError;
        if (sourceImfHashReady) {
            loaded->visualIndexReady = MapVisualIndexCodec::Load(
                featureRoot / "Map_visual_index.imx", sourceImfSha,
                static_cast<std::uint32_t>(loaded->map.imgKeypoints.size()),
                loaded->visualIndex, visualError);
            if (loaded->visualIndexReady) {
                loaded->baseVisualTileCount = static_cast<std::uint32_t>(loaded->visualIndex.tiles.size());
            }
        }
        else {
            visualError = failure;
        }

        if (!FeatureLoader::loadFeatures((featureRoot / "IconTask_Features.yml").string(), loaded->iconTask) ||
            !FeatureLoader::loadFeatures((featureRoot / "IconWavePlateCrystal_Features.yml").string(), loaded->wavePlateCrystal)) {
            throw std::runtime_error("icon feature resources failed to load");
        }

        const auto kuroPacks = KuroTileFeaturePack::LoadRegistered(featureRoot.string());
        for (const auto& kuro : kuroPacks) {
            if (kuro.loaded && kuro.runtimeApproved) {
                const auto rowBase = static_cast<std::uint32_t>(loaded->map.imgKeypoints.size());
                MapVisualIndex shard;
                const auto firstShardTile = static_cast<std::uint32_t>(loaded->visualIndex.tiles.size());
                std::string shardError;
                const bool shardReady = loaded->visualIndexReady &&
                    MapVisualIndexCodec::Load(featureRoot / "KuroTilePacks" / kuro.directoryName / "visual-index.imx",
                        kuro.sourceSha256, static_cast<std::uint32_t>(kuro.featureData.imgKeypoints.size()),
                        shard, shardError) &&
                    MergeVisualShard(loaded->visualIndex, shard, rowBase, shardError);
                if (!shardReady) {
                    loaded->visualIndexReady = false;
                    visualError += " Kuro shard " + kuro.directoryName + ": " + shardError;
                }
                else {
                    loaded->kuroVisualShards.push_back({
                        kuro.sceneId, firstShardTile, static_cast<std::uint32_t>(shard.tiles.size()) });
                }
                CandidateFeaturePack::AppendFeatures(loaded->map, kuro.featureData);
                CandidateFeaturePack::AppendFeatures(loaded->kuroTileFeatures, kuro.featureData);
            }
            Diagnostics::Record("kuro-tile-feature-pack", "id=" + kuro.packId +
                " directory=" + kuro.directoryName + " scene=" + std::to_string(kuro.sceneId) +
                " loaded=" + std::to_string(kuro.loaded) + " binary=" +
                std::to_string(kuro.loadedFromBinary) + " approved=" +
                std::to_string(kuro.runtimeApproved) + " keypoints=" + std::to_string(kuro.keypointCount) +
                " error=" + kuro.error);
        }

        const auto candidates = CandidateFeaturePack::LoadRegisteredCandidates(featureRoot.string());
        for (auto candidate : candidates) {
            if (candidate.loaded) {
            const auto rowBase = static_cast<std::uint32_t>(loaded->map.imgKeypoints.size());
            std::array<std::uint8_t, 32> sourceHash{};
            MapVisualIndex shard;
            std::string shardError;
            const auto sourcePath = featureRoot / candidate.directoryName / "manifest.json";
            const bool shardReady = loaded->visualIndexReady &&
                FeatureBinaryCodec::Sha256File(sourcePath, sourceHash, shardError) &&
                MapVisualIndexCodec::Load(featureRoot / candidate.directoryName / "visual-index.imx",
                    sourceHash, static_cast<std::uint32_t>(candidate.featureData.imgKeypoints.size()),
                    shard, shardError) &&
                MergeVisualShard(loaded->visualIndex, shard, rowBase, shardError);
            if (!shardReady) {
                loaded->visualIndexReady = false;
                visualError += " candidate shard " + candidate.packId + ": " + shardError;
            }
            CandidateFeaturePack::AppendFeatures(loaded->map, candidate.featureData);
            CandidateFeaturePack::AppendFeatures(loaded->curatedCandidates, candidate.featureData);
            }
            Diagnostics::Record("candidate-feature-pack", "id=" + candidate.packId +
                " directory=" + candidate.directoryName + " loaded=" + std::to_string(candidate.loaded) +
                " references=" + std::to_string(candidate.referenceCount) + " keypoints=" +
                std::to_string(candidate.keypointCount) + " selfAccepted=" +
                std::to_string(candidate.selfMatchAccepted) + " error=" + candidate.error);
        }

        Diagnostics::Record("resource-load", "stage=visual-index durationMs=" +
            std::to_string(ElapsedMilliseconds(visualStart)) + " ready=" +
            std::to_string(loaded->visualIndexReady) + " tiles=" +
            std::to_string(loaded->visualIndex.tiles.size()) + " error=" + visualError);
    }
    catch (const std::exception& exception) {
        failure = exception.what();
        loaded.reset();
    }

    const bool ready = loaded != nullptr;
    {
        std::scoped_lock lock(mutex_);
        resources_ = std::move(loaded);
        error_ = std::move(failure);
        completed_ = true;
    }
    Diagnostics::Record("resource-load", "stage=all durationMs=" +
        std::to_string(ElapsedMilliseconds(totalStart)) + " ready=" +
        std::to_string(ready));
    condition_.notify_all();
}
