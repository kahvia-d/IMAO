#include "LegacyFeatureExclusions.h"
#include "RuntimeFeatureRepository.h"

#include "CandidateFeaturePack.h"
#include "KuroTileFeaturePack.h"
#include "Processing/FeatureBinaryCodec.h"
#include "Processing/FeatureProcessing.h"
#include "VisualIndex/MapVisualIndex.h"
#include "../Diagnostics/Diagnostics.h"
#include "../Runtime/ThreadPriority.h"
#include "../Runtime/RuntimeStatus.h"

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
    base.tiles.reserve(base.tiles.size() + shard.tiles.size());
    base.histograms.reserve(base.histograms.size() + shard.histograms.size());
    base.featureRows.reserve(base.featureRows.size() + shard.featureRows.size());
    base.postings.reserve(base.postings.size() + shard.postings.size());
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
    base.featureCount += shard.featureCount;
    return true;
}

bool FinalizeMergedVisualIndex(MapVisualIndex& index, std::string& error) {
    std::sort(index.postings.begin(), index.postings.end(), [](const auto& left, const auto& right) {
        if (left.wordId != right.wordId) return left.wordId < right.wordId;
        return left.tileIndex < right.tileIndex;
    });
    return MapVisualIndexCodec::BuildPostingOffsets(index, error);
}

// Repeated cv::vconcat calls reallocate and copy the large base map descriptor
// matrix for every optional pack.  On lower-memory machines that causes paging
// and makes a few small extension packs appear to hang the application.
bool AppendFeatureBatch(ImageFeatureData& destination,
    const std::vector<const ImageFeatureData*>& additions, std::string& error) {
    if (additions.empty()) return true;
    if (destination.imgKeypoints.empty() || destination.imgDescriptors.empty() ||
        destination.imgDescriptors.rows != static_cast<int>(destination.imgKeypoints.size())) {
        error = "base map feature data is incomplete";
        return false;
    }

    std::size_t totalKeypoints = destination.imgKeypoints.size();
    for (const auto* addition : additions) {
        if (addition == nullptr || addition->imgKeypoints.empty() || addition->imgDescriptors.empty()) continue;
        if (addition->imgDescriptors.rows != static_cast<int>(addition->imgKeypoints.size()) ||
            addition->imgDescriptors.type() != destination.imgDescriptors.type() ||
            addition->imgDescriptors.cols != destination.imgDescriptors.cols) {
            error = "optional map feature descriptors are incompatible with the base map";
            return false;
        }
        if (addition->imgKeypoints.size() >
            std::numeric_limits<std::size_t>::max() - totalKeypoints) {
            error = "optional map feature count overflow";
            return false;
        }
        totalKeypoints += addition->imgKeypoints.size();
    }
    if (totalKeypoints > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        error = "optional map feature row count exceeds OpenCV limits";
        return false;
    }

    ImageFeatureData merged;
    merged.imgKeypoints.reserve(totalKeypoints);
    merged.imgDescriptors.create(static_cast<int>(totalKeypoints), destination.imgDescriptors.cols,
        destination.imgDescriptors.type());
    int row = 0;
    const auto append = [&merged, &row](const ImageFeatureData& source) {
        merged.imgKeypoints.insert(merged.imgKeypoints.end(), source.imgKeypoints.begin(), source.imgKeypoints.end());
        const auto nextRow = row + source.imgDescriptors.rows;
        source.imgDescriptors.copyTo(merged.imgDescriptors.rowRange(row, nextRow));
        row = nextRow;
    };
    append(destination);
    for (const auto* addition : additions) {
        if (addition != nullptr && !addition->imgKeypoints.empty() && !addition->imgDescriptors.empty()) {
            append(*addition);
        }
    }
    destination = std::move(merged);
    return true;
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
    ThreadPriority::MakeBackground(preloadThread_);
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
        const auto featureRoot = (ResourceSnapshotContext::Configured() ? ResourceSnapshotContext::BaselineRoot() : assetRoot) / "FeaturesDatas";
        // The base map features may live in their own package; MapFeatureRoot falls back to
        // featureRoot, so this is the same directory for every layout written before the split.
        const auto mapFeatureRoot = ResourceSnapshotContext::Configured()
            ? ResourceSnapshotContext::MapFeatureRoot() : featureRoot;
        const auto mapStart = std::chrono::steady_clock::now();
        std::array<std::uint8_t, 32> sourceImfSha{};
        bool sourceImfHashReady = FeatureBinaryCodec::Load(
            mapFeatureRoot / "Map_features.imf", loaded->map, failure, nullptr, &sourceImfSha);
        const auto baselineRows = loaded->map.imgKeypoints.size();
        if (!sourceImfHashReady) {
#ifdef IMAO_ALLOW_XML_FEATURE_FALLBACK
            Diagnostics::Record("resource-load", "stage=map-imf failed fallback=xml error=" + failure);
            if (!FeatureLoader::loadFeaturesFromXML((mapFeatureRoot / "Map_features.yml").string(), loaded->map)) {
                throw std::runtime_error("map IMF and XML fallback both failed: " + failure);
            }
#else
            throw std::runtime_error(failure);
#endif
        }
        Diagnostics::Record("resource-load", "stage=map-features durationMs=" +
            std::to_string(ElapsedMilliseconds(mapStart)) + " keypoints=" +
            std::to_string(loaded->map.imgKeypoints.size()));
        RuntimeStatus::SetMessage("基础地图特征已就绪，正在读取地图索引与图标资源");
        if (stopToken.stop_requested()) throw std::runtime_error("feature preload cancelled");

        const auto visualStart = std::chrono::steady_clock::now();
        std::string visualError;
        if (sourceImfHashReady) {
            loaded->visualIndexReady = MapVisualIndexCodec::Load(
                mapFeatureRoot / "Map_visual_index.imx", sourceImfSha,
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

        RuntimeStatus::SetMessage("正在加载扩展地图特征包，请稍候");
        const auto kuroLoadStart = std::chrono::steady_clock::now();
        const auto kuroPacks = KuroTileFeaturePack::LoadRegistered(featureRoot.string());
        Diagnostics::Record("resource-load", "stage=kuro-feature-packs durationMs=" +
            std::to_string(ElapsedMilliseconds(kuroLoadStart)) + " packs=" + std::to_string(kuroPacks.size()));
        std::vector<const ImageFeatureData*> mapFeatureAdditions;
        std::uint32_t mergedFeatureRows = static_cast<std::uint32_t>(loaded->map.imgKeypoints.size());
        const auto approvedKuroPackCount = static_cast<std::size_t>(std::count_if(
            kuroPacks.begin(), kuroPacks.end(), [](const auto& pack) { return pack.loaded && pack.runtimeApproved; }));
        std::size_t loadedKuroPack = 0;
        for (const auto& kuro : kuroPacks) {
            if (ResourceSnapshotContext::Configured() && (!kuro.loaded || !kuro.runtimeApproved))
                throw std::runtime_error("selected tile package failed: " + kuro.directoryPath.string() + " " + kuro.error);
            if (kuro.loaded && kuro.runtimeApproved) {
                ++loadedKuroPack;
                RuntimeStatus::SetMessage("正在整合扩展地图索引：" + kuro.directoryName + "（" +
                    std::to_string(loadedKuroPack) + "/" + std::to_string(approvedKuroPackCount) + "）");
                const auto kuroMergeStart = std::chrono::steady_clock::now();
                const auto rowBase = mergedFeatureRows;
                MapVisualIndex shard;
                const auto firstShardTile = static_cast<std::uint32_t>(loaded->visualIndex.tiles.size());
                std::string shardError;
                const bool shardReady = loaded->visualIndexReady &&
                    MapVisualIndexCodec::Load(kuro.directoryPath / "visual-index.imx",
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
                if (kuro.featureData.imgKeypoints.size() >
                    std::numeric_limits<std::uint32_t>::max() - mergedFeatureRows) {
                    throw std::runtime_error("Kuro tile feature row count overflow");
                }
                ApplyLegacyFeatureExclusions(*loaded, kuro.directoryPath / "manifest.json", sourceImfSha, baselineRows);
                mapFeatureAdditions.push_back(&kuro.featureData);
                mergedFeatureRows += static_cast<std::uint32_t>(kuro.featureData.imgKeypoints.size());
                CandidateFeaturePack::AppendFeatures(loaded->kuroTileFeatures, kuro.featureData);
                Diagnostics::Record("kuro-tile-feature-index", "id=" + kuro.packId +
                    " durationMs=" + std::to_string(ElapsedMilliseconds(kuroMergeStart)) +
                    " visualIndexReady=" + std::to_string(shardReady) +
                    " keypoints=" + std::to_string(kuro.featureData.imgKeypoints.size()) +
                    " error=" + shardError);
            }
            Diagnostics::Record("kuro-tile-feature-pack", "id=" + kuro.packId +
                " directory=" + kuro.directoryName + " scene=" + std::to_string(kuro.sceneId) +
                " loaded=" + std::to_string(kuro.loaded) + " binary=" +
                std::to_string(kuro.loadedFromBinary) + " approved=" +
                std::to_string(kuro.runtimeApproved) + " keypoints=" + std::to_string(kuro.keypointCount) +
                " error=" + kuro.error);
        }

        RuntimeStatus::SetMessage("正在加载候选地图定位资源");
        const auto candidateLoadStart = std::chrono::steady_clock::now();
        const auto candidates = CandidateFeaturePack::LoadRegisteredCandidates(featureRoot.string());
        Diagnostics::Record("resource-load", "stage=candidate-feature-packs durationMs=" +
            std::to_string(ElapsedMilliseconds(candidateLoadStart)) + " packs=" + std::to_string(candidates.size()));
        std::size_t loadedCandidatePack = 0;
        for (const auto& candidate : candidates) {
            if (ResourceSnapshotContext::Configured() && !candidate.loaded)
                throw std::runtime_error("selected candidate package failed: " + candidate.directoryPath.string() + " " + candidate.error);
            if (candidate.loaded) {
                ++loadedCandidatePack;
                RuntimeStatus::SetMessage("正在整合候选地图索引：" + candidate.directoryName + "（" +
                    std::to_string(loadedCandidatePack) + "/" + std::to_string(candidates.size()) + "）");
                const auto candidateMergeStart = std::chrono::steady_clock::now();
                const auto rowBase = mergedFeatureRows;
                MapVisualIndex shard;
                std::string shardError;
                const auto sourcePath = candidate.directoryPath / "manifest.json";
                const bool shardReady = loaded->visualIndexReady &&
                    MapVisualIndexCodec::LoadManifestShard(candidate.directoryPath / "visual-index.imx",
                        sourcePath, static_cast<std::uint32_t>(candidate.featureData.imgKeypoints.size()),
                        shard, shardError) &&
                    MergeVisualShard(loaded->visualIndex, shard, rowBase, shardError);
                if (!shardReady) {
                    loaded->visualIndexReady = false;
                    visualError += " candidate shard " + candidate.packId + ": " + shardError;
                }
                if (candidate.featureData.imgKeypoints.size() >
                    std::numeric_limits<std::uint32_t>::max() - mergedFeatureRows) {
                    throw std::runtime_error("candidate feature row count overflow");
                }
                mapFeatureAdditions.push_back(&candidate.featureData);
                mergedFeatureRows += static_cast<std::uint32_t>(candidate.featureData.imgKeypoints.size());
                CandidateFeaturePack::AppendFeatures(loaded->curatedCandidates, candidate.featureData);
                Diagnostics::Record("candidate-feature-index", "id=" + candidate.packId +
                    " durationMs=" + std::to_string(ElapsedMilliseconds(candidateMergeStart)) +
                    " visualIndexReady=" + std::to_string(shardReady) +
                    " keypoints=" + std::to_string(candidate.featureData.imgKeypoints.size()) +
                    " error=" + shardError);
            }
            Diagnostics::Record("candidate-feature-pack", "id=" + candidate.packId +
                " directory=" + candidate.directoryName + " loaded=" + std::to_string(candidate.loaded) +
                " references=" + std::to_string(candidate.referenceCount) + " keypoints=" +
                std::to_string(candidate.keypointCount) + " selfAccepted=" +
                std::to_string(candidate.selfMatchAccepted) + " error=" + candidate.error);
        }

        if (!mapFeatureAdditions.empty()) {
            RuntimeStatus::SetMessage("正在一次性合并全部地图特征");
            const auto mergeStart = std::chrono::steady_clock::now();
            std::string mergeError;
            if (!AppendFeatureBatch(loaded->map, mapFeatureAdditions, mergeError)) {
                throw std::runtime_error("map feature merge failed: " + mergeError);
            }
            Diagnostics::Record("resource-load", "stage=map-feature-merge durationMs=" +
                std::to_string(ElapsedMilliseconds(mergeStart)) + " additions=" +
                std::to_string(mapFeatureAdditions.size()) + " keypoints=" +
                std::to_string(loaded->map.imgKeypoints.size()));
        }

        if (loaded->visualIndexReady) {
            RuntimeStatus::SetMessage("正在整理地图定位索引");
            const auto finalizeStart = std::chrono::steady_clock::now();
            std::string finalizeError;
            if (!FinalizeMergedVisualIndex(loaded->visualIndex, finalizeError)) {
                loaded->visualIndexReady = false;
                visualError += " visual-index finalization: " + finalizeError;
            }
            Diagnostics::Record("resource-load", "stage=visual-index-finalize durationMs=" +
                std::to_string(ElapsedMilliseconds(finalizeStart)) + " ready=" +
                std::to_string(loaded->visualIndexReady) + " postings=" +
                std::to_string(loaded->visualIndex.postings.size()) + " error=" + finalizeError);
        }

        Diagnostics::Record("resource-load", "stage=visual-index durationMs=" +
            std::to_string(ElapsedMilliseconds(visualStart)) + " ready=" +
            std::to_string(loaded->visualIndexReady) + " tiles=" +
            std::to_string(loaded->visualIndex.tiles.size()) + " error=" + visualError);
        if (!loaded->visualIndexReady) {
            throw std::runtime_error("地图视觉索引不可用：" + visualError);
        }
        RuntimeStatus::SetMessage("地图识别资源已就绪");
    }
    catch (const std::exception& exception) {
        failure = exception.what();
        Diagnostics::Record("resource-load-failed", "stage=preload error=" + failure);
        RuntimeStatus::SetCoreState("faulted", "地图资源加载失败：" + failure);
        loaded.reset();
    }

    const bool ready = loaded != nullptr;
    const std::string failureForLog = failure;
    {
        std::scoped_lock lock(mutex_);
        resources_ = std::move(loaded);
        error_ = std::move(failure);
        completed_ = true;
    }
    Diagnostics::Record("resource-load", "stage=all durationMs=" +
        std::to_string(ElapsedMilliseconds(totalStart)) + " ready=" +
        std::to_string(ready) + " error=" + failureForLog);
    condition_.notify_all();
}
