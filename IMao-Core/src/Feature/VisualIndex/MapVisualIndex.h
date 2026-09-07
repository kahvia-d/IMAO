#pragma once

#include <opencv2/core.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct MapVisualTile {
    std::int32_t sceneId = 0;
    std::int32_t gridX = 0;
    std::int32_t gridY = 0;
    float minX = 0.0f;
    float minY = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
    std::uint32_t histogramOffset = 0;
    std::uint32_t histogramCount = 0;
    std::uint32_t featureRowOffset = 0;
    std::uint32_t featureRowCount = 0;
    float histogramNorm = 0.0f;
};

struct MapVisualHistogramEntry {
    std::uint32_t wordId = 0;
    float weight = 0.0f;
};

struct MapVisualPosting {
    std::uint32_t wordId = 0;
    std::uint32_t tileIndex = 0;
    float weight = 0.0f;
};

struct MapVisualIndex {
    static constexpr std::uint32_t WordCount = 4096;
    static constexpr std::uint32_t DescriptorColumns = 128;
    static constexpr std::uint32_t TileSize = 384;
    static constexpr std::uint32_t TileStride = 192;
    static constexpr std::uint32_t VerificationMargin = 96;

    cv::Mat vocabulary;
    std::vector<MapVisualTile> tiles;
    std::vector<MapVisualHistogramEntry> histograms;
    std::vector<std::uint32_t> featureRows;
    std::vector<MapVisualPosting> postings;
    std::vector<std::uint32_t> postingOffsets;
    std::array<std::uint8_t, 32> sourceImfSha256{};
    std::array<std::uint8_t, 32> vocabularySha256{};
    std::uint32_t featureCount = 0;
};

struct MapVisualIndexHeader {
    static constexpr std::array<char, 8> Magic = { 'I', 'M', 'A', 'O', 'I', 'X', '0', '1' };
    static constexpr std::uint32_t CurrentVersion = 1;
    static constexpr std::uint32_t LittleEndianMarker = 0x01020304;
    static constexpr std::uint32_t SerializedSize = 192;

    std::uint32_t version = CurrentVersion;
    std::uint32_t headerLength = SerializedSize;
    std::uint32_t endianMarker = LittleEndianMarker;
    std::uint32_t wordCount = MapVisualIndex::WordCount;
    std::uint32_t descriptorColumns = MapVisualIndex::DescriptorColumns;
    std::uint32_t tileSize = MapVisualIndex::TileSize;
    std::uint32_t tileStride = MapVisualIndex::TileStride;
    std::uint32_t featureCount = 0;
    std::uint32_t tileCount = 0;
    std::uint32_t histogramEntryCount = 0;
    std::uint32_t featureRowCount = 0;
    std::uint32_t postingCount = 0;
    std::uint64_t vocabularyPayloadLength = 0;
    std::uint64_t tilePayloadLength = 0;
    std::uint64_t histogramPayloadLength = 0;
    std::uint64_t featureRowPayloadLength = 0;
    std::uint64_t postingPayloadLength = 0;
    std::array<std::uint8_t, 32> sourceImfSha256{};
    std::array<std::uint8_t, 32> vocabularySha256{};
    std::array<std::uint8_t, 32> payloadSha256{};
};

class MapVisualIndexCodec {
public:
    // Accept the original file bytes or CRLF-to-LF checkout conversion only.
    static bool LoadManifestShard(const std::filesystem::path& path,
        const std::filesystem::path& manifest, std::uint32_t expectedFeatureCount,
        MapVisualIndex& output, std::string& error);
    static bool Save(const std::filesystem::path& path, const MapVisualIndex& index,
        std::string& error, MapVisualIndexHeader* header = nullptr);

    static bool Load(const std::filesystem::path& path,
        const std::array<std::uint8_t, 32>& expectedImfSha256,
        std::uint32_t expectedFeatureCount, MapVisualIndex& output,
        std::string& error, MapVisualIndexHeader* header = nullptr);

    static bool BuildPostingOffsets(MapVisualIndex& index, std::string& error);
};

class MapVisualIndexBuilder {
public:
    static bool Build(const std::vector<cv::KeyPoint>& keypoints,
        const cv::Mat& descriptors,
        const std::array<std::uint8_t, 32>& sourceImfSha256,
        MapVisualIndex& output, std::string& error,
        const cv::Mat& fixedVocabulary = cv::Mat(), int forcedSceneId = 0);
};
