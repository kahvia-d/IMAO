#include "MapVisualIndex.h"

#include "../Processing/FeatureBinaryCodec.h"
#include "../../Coordinate/CoordinateStruct.h"

#include <bcrypt.h>
#include <opencv2/flann.hpp>
#include <opencv2/flann/random.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <type_traits>

namespace {
constexpr std::uint32_t kTileSerializedSize = 48;
constexpr std::uint32_t kHistogramSerializedSize = 8;
constexpr std::uint32_t kPostingSerializedSize = 12;
constexpr std::uint32_t kMaximumTiles = 250'000;
constexpr std::uint32_t kMaximumEntries = 100'000'000;

class Sha256State {
public:
    Sha256State() {
        DWORD objectLength = 0;
        DWORD hashLength = 0;
        DWORD received = 0;
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) ||
            !BCRYPT_SUCCESS(BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &received, 0)) ||
            !BCRYPT_SUCCESS(BCryptGetProperty(algorithm_, BCRYPT_HASH_LENGTH,
                reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &received, 0)) || hashLength != 32) {
            throw std::runtime_error("SHA-256 provider initialization failed");
        }
        object_.resize(objectLength);
        if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm_, &hash_, object_.data(), objectLength, nullptr, 0, 0))) {
            throw std::runtime_error("SHA-256 hash initialization failed");
        }
    }

    ~Sha256State() {
        if (hash_ != nullptr) BCryptDestroyHash(hash_);
        if (algorithm_ != nullptr) BCryptCloseAlgorithmProvider(algorithm_, 0);
    }

    void Update(const void* data, std::size_t size) {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        while (size > 0) {
            const auto chunk = static_cast<ULONG>(std::min<std::size_t>(size, std::numeric_limits<ULONG>::max()));
            if (!BCRYPT_SUCCESS(BCryptHashData(hash_, const_cast<PUCHAR>(bytes), chunk, 0))) {
                throw std::runtime_error("SHA-256 update failed");
            }
            bytes += chunk;
            size -= chunk;
        }
    }

    std::array<std::uint8_t, 32> Finish() {
        std::array<std::uint8_t, 32> value{};
        if (!BCRYPT_SUCCESS(BCryptFinishHash(hash_, value.data(), static_cast<ULONG>(value.size()), 0))) {
            throw std::runtime_error("SHA-256 finish failed");
        }
        BCryptDestroyHash(hash_);
        hash_ = nullptr;
        return value;
    }

private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    std::vector<std::uint8_t> object_;
};

template <typename T>
void AppendLittleEndian(std::vector<std::uint8_t>& output, T value) {
    static_assert(std::is_integral_v<T>);
    using Unsigned = std::make_unsigned_t<T>;
    const auto bits = static_cast<Unsigned>(value);
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        output.push_back(static_cast<std::uint8_t>((bits >> (index * 8)) & 0xff));
    }
}

void AppendFloat(std::vector<std::uint8_t>& output, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    AppendLittleEndian(output, bits);
}

template <typename T>
bool ReadLittleEndian(const std::vector<std::uint8_t>& input, std::size_t& offset, T& output) {
    static_assert(std::is_integral_v<T>);
    if (offset > input.size() || input.size() - offset < sizeof(T)) return false;
    using Unsigned = std::make_unsigned_t<T>;
    Unsigned value = 0;
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        value |= static_cast<Unsigned>(input[offset++]) << (index * 8);
    }
    output = static_cast<T>(value);
    return true;
}

bool ReadFloat(const std::vector<std::uint8_t>& input, std::size_t& offset, float& output) {
    std::uint32_t bits = 0;
    if (!ReadLittleEndian(input, offset, bits)) return false;
    std::memcpy(&output, &bits, sizeof(output));
    return std::isfinite(output);
}

template <typename T>
bool CheckedMultiply(T first, T second, T& output) {
    if (first != 0 && second > std::numeric_limits<T>::max() / first) return false;
    output = first * second;
    return true;
}

bool CheckedAdd(std::uint64_t first, std::uint64_t second, std::uint64_t& output) {
    if (second > std::numeric_limits<std::uint64_t>::max() - first) return false;
    output = first + second;
    return true;
}

std::array<std::uint8_t, 32> HashBytes(const std::vector<std::uint8_t>& bytes) {
    Sha256State hash;
    if (!bytes.empty()) hash.Update(bytes.data(), bytes.size());
    return hash.Finish();
}

std::vector<std::uint8_t> SerializeVocabulary(const cv::Mat& vocabulary) {
    std::vector<std::uint8_t> output;
    output.reserve(vocabulary.total() * sizeof(float));
    for (int row = 0; row < vocabulary.rows; ++row) {
        const auto* values = vocabulary.ptr<float>(row);
        for (int column = 0; column < vocabulary.cols; ++column) AppendFloat(output, values[column]);
    }
    return output;
}

std::vector<std::uint8_t> SerializeTiles(const std::vector<MapVisualTile>& tiles) {
    std::vector<std::uint8_t> output;
    output.reserve(tiles.size() * kTileSerializedSize);
    for (const auto& tile : tiles) {
        AppendLittleEndian(output, tile.sceneId);
        AppendLittleEndian(output, tile.gridX);
        AppendLittleEndian(output, tile.gridY);
        AppendFloat(output, tile.minX);
        AppendFloat(output, tile.minY);
        AppendFloat(output, tile.maxX);
        AppendFloat(output, tile.maxY);
        AppendLittleEndian(output, tile.histogramOffset);
        AppendLittleEndian(output, tile.histogramCount);
        AppendLittleEndian(output, tile.featureRowOffset);
        AppendLittleEndian(output, tile.featureRowCount);
        AppendFloat(output, tile.histogramNorm);
    }
    return output;
}

std::vector<std::uint8_t> SerializeHistograms(const std::vector<MapVisualHistogramEntry>& entries) {
    std::vector<std::uint8_t> output;
    output.reserve(entries.size() * kHistogramSerializedSize);
    for (const auto& entry : entries) {
        AppendLittleEndian(output, entry.wordId);
        AppendFloat(output, entry.weight);
    }
    return output;
}

std::vector<std::uint8_t> SerializeRows(const std::vector<std::uint32_t>& rows) {
    std::vector<std::uint8_t> output;
    output.reserve(rows.size() * sizeof(std::uint32_t));
    for (const auto row : rows) AppendLittleEndian(output, row);
    return output;
}

std::vector<std::uint8_t> SerializePostings(const std::vector<MapVisualPosting>& postings) {
    std::vector<std::uint8_t> output;
    output.reserve(postings.size() * kPostingSerializedSize);
    for (const auto& posting : postings) {
        AppendLittleEndian(output, posting.wordId);
        AppendLittleEndian(output, posting.tileIndex);
        AppendFloat(output, posting.weight);
    }
    return output;
}

std::vector<std::uint8_t> SerializeHeader(const MapVisualIndexHeader& header) {
    std::vector<std::uint8_t> output;
    output.reserve(MapVisualIndexHeader::SerializedSize);
    output.insert(output.end(), MapVisualIndexHeader::Magic.begin(), MapVisualIndexHeader::Magic.end());
    AppendLittleEndian(output, header.version);
    AppendLittleEndian(output, header.headerLength);
    AppendLittleEndian(output, header.endianMarker);
    AppendLittleEndian(output, header.wordCount);
    AppendLittleEndian(output, header.descriptorColumns);
    AppendLittleEndian(output, header.tileSize);
    AppendLittleEndian(output, header.tileStride);
    AppendLittleEndian(output, header.featureCount);
    AppendLittleEndian(output, header.tileCount);
    AppendLittleEndian(output, header.histogramEntryCount);
    AppendLittleEndian(output, header.featureRowCount);
    AppendLittleEndian(output, header.postingCount);
    AppendLittleEndian(output, header.vocabularyPayloadLength);
    AppendLittleEndian(output, header.tilePayloadLength);
    AppendLittleEndian(output, header.histogramPayloadLength);
    AppendLittleEndian(output, header.featureRowPayloadLength);
    AppendLittleEndian(output, header.postingPayloadLength);
    output.insert(output.end(), header.sourceImfSha256.begin(), header.sourceImfSha256.end());
    output.insert(output.end(), header.vocabularySha256.begin(), header.vocabularySha256.end());
    output.insert(output.end(), header.payloadSha256.begin(), header.payloadSha256.end());
    return output;
}

bool DeserializeHeader(const std::vector<std::uint8_t>& bytes, MapVisualIndexHeader& header) {
    if (bytes.size() != MapVisualIndexHeader::SerializedSize ||
        !std::equal(MapVisualIndexHeader::Magic.begin(), MapVisualIndexHeader::Magic.end(), bytes.begin())) return false;
    std::size_t offset = MapVisualIndexHeader::Magic.size();
    if (!ReadLittleEndian(bytes, offset, header.version) ||
        !ReadLittleEndian(bytes, offset, header.headerLength) ||
        !ReadLittleEndian(bytes, offset, header.endianMarker) ||
        !ReadLittleEndian(bytes, offset, header.wordCount) ||
        !ReadLittleEndian(bytes, offset, header.descriptorColumns) ||
        !ReadLittleEndian(bytes, offset, header.tileSize) ||
        !ReadLittleEndian(bytes, offset, header.tileStride) ||
        !ReadLittleEndian(bytes, offset, header.featureCount) ||
        !ReadLittleEndian(bytes, offset, header.tileCount) ||
        !ReadLittleEndian(bytes, offset, header.histogramEntryCount) ||
        !ReadLittleEndian(bytes, offset, header.featureRowCount) ||
        !ReadLittleEndian(bytes, offset, header.postingCount) ||
        !ReadLittleEndian(bytes, offset, header.vocabularyPayloadLength) ||
        !ReadLittleEndian(bytes, offset, header.tilePayloadLength) ||
        !ReadLittleEndian(bytes, offset, header.histogramPayloadLength) ||
        !ReadLittleEndian(bytes, offset, header.featureRowPayloadLength) ||
        !ReadLittleEndian(bytes, offset, header.postingPayloadLength)) return false;
    if (offset + 96 != bytes.size()) return false;
    std::copy_n(bytes.begin() + offset, 32, header.sourceImfSha256.begin());
    offset += 32;
    std::copy_n(bytes.begin() + offset, 32, header.vocabularySha256.begin());
    offset += 32;
    std::copy_n(bytes.begin() + offset, 32, header.payloadSha256.begin());
    return true;
}

std::int32_t NearestScene(const cv::Point2f point) {
    static constexpr std::array<std::tuple<std::int32_t, float, float>, 5> origins = {
        std::tuple{1, 2474.0f, 1957.0f}, std::tuple{2, 8593.0f, 1382.0f},
        std::tuple{3, 7437.0f, 13783.0f}, std::tuple{4, 2433.0f, 9030.0f},
        std::tuple{5, 21662.0f, 13138.0f}
    };
    std::int32_t bestScene = 0;
    double bestDistance = std::numeric_limits<double>::infinity();
    for (const auto& [scene, x, y] : origins) {
        const double dx = static_cast<double>(point.x) - x;
        const double dy = static_cast<double>(point.y) - y;
        const double distance = dx * dx + dy * dy;
        if (distance < bestDistance) {
            bestDistance = distance;
            bestScene = scene;
        }
    }
    return bestScene;
}

int FloorGrid(float value) {
    return static_cast<int>(std::floor(value / static_cast<float>(MapVisualIndex::TileStride)));
}

struct TileBuildData {
    std::int32_t sceneId = 0;
    std::int32_t gridX = 0;
    std::int32_t gridY = 0;
    std::map<std::uint32_t, std::uint32_t> wordCounts;
    std::vector<std::uint32_t> verificationRows;
    std::uint32_t coreFeatureCount = 0;
};

using TileKey = std::tuple<std::int32_t, std::int32_t, std::int32_t>;

bool IsValidFeatureInput(const std::vector<cv::KeyPoint>& keypoints, const cv::Mat& descriptors,
    std::string& error) {
    if (keypoints.empty() || descriptors.empty() || descriptors.type() != CV_32FC1 ||
        descriptors.cols != static_cast<int>(MapVisualIndex::DescriptorColumns) ||
        descriptors.rows != static_cast<int>(keypoints.size())) {
        error = "visual index input must contain matching CV_32FC1 128-column features";
        return false;
    }
    if (keypoints.size() > std::numeric_limits<std::uint32_t>::max()) {
        error = "visual index feature count is too large";
        return false;
    }
    return true;
}
}

bool MapVisualIndexCodec::BuildPostingOffsets(MapVisualIndex& index, std::string& error) {
    index.postingOffsets.assign(MapVisualIndex::WordCount + 1, 0);
    std::uint32_t previousWord = 0;
    bool first = true;
    for (std::uint32_t postingIndex = 0; postingIndex < index.postings.size(); ++postingIndex) {
        const auto& posting = index.postings[postingIndex];
        if (posting.wordId >= MapVisualIndex::WordCount || posting.tileIndex >= index.tiles.size() ||
            !std::isfinite(posting.weight) || posting.weight <= 0.0f) {
            error = "visual index posting is invalid";
            return false;
        }
        if (!first && posting.wordId < previousWord) {
            error = "visual index postings are not sorted by word";
            return false;
        }
        previousWord = posting.wordId;
        first = false;
    }
    std::uint32_t postingIndex = 0;
    for (std::uint32_t word = 0; word < MapVisualIndex::WordCount; ++word) {
        index.postingOffsets[word] = postingIndex;
        while (postingIndex < index.postings.size() && index.postings[postingIndex].wordId == word) {
            ++postingIndex;
        }
    }
    index.postingOffsets[MapVisualIndex::WordCount] = static_cast<std::uint32_t>(index.postings.size());
    return true;
}

bool MapVisualIndexCodec::Save(const std::filesystem::path& path, const MapVisualIndex& index,
    std::string& error, MapVisualIndexHeader* returnedHeader) {
    try {
        if (index.vocabulary.type() != CV_32FC1 ||
            index.vocabulary.rows != static_cast<int>(MapVisualIndex::WordCount) ||
            index.vocabulary.cols != static_cast<int>(MapVisualIndex::DescriptorColumns) ||
            index.featureCount == 0 || index.tiles.empty()) {
            error = "visual index is incomplete";
            return false;
        }

        const auto vocabularyBytes = SerializeVocabulary(index.vocabulary);
        const auto tileBytes = SerializeTiles(index.tiles);
        const auto histogramBytes = SerializeHistograms(index.histograms);
        const auto rowBytes = SerializeRows(index.featureRows);
        const auto postingBytes = SerializePostings(index.postings);

        Sha256State payloadHash;
        for (const auto* bytes : { &vocabularyBytes, &tileBytes, &histogramBytes, &rowBytes, &postingBytes }) {
            if (!bytes->empty()) payloadHash.Update(bytes->data(), bytes->size());
        }

        MapVisualIndexHeader header;
        header.featureCount = index.featureCount;
        header.tileCount = static_cast<std::uint32_t>(index.tiles.size());
        header.histogramEntryCount = static_cast<std::uint32_t>(index.histograms.size());
        header.featureRowCount = static_cast<std::uint32_t>(index.featureRows.size());
        header.postingCount = static_cast<std::uint32_t>(index.postings.size());
        header.vocabularyPayloadLength = vocabularyBytes.size();
        header.tilePayloadLength = tileBytes.size();
        header.histogramPayloadLength = histogramBytes.size();
        header.featureRowPayloadLength = rowBytes.size();
        header.postingPayloadLength = postingBytes.size();
        header.sourceImfSha256 = index.sourceImfSha256;
        header.vocabularySha256 = HashBytes(vocabularyBytes);
        header.payloadSha256 = payloadHash.Finish();

        const auto headerBytes = SerializeHeader(header);
        if (headerBytes.size() != MapVisualIndexHeader::SerializedSize) {
            error = "visual index header serialization size mismatch";
            return false;
        }
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "unable to create visual index";
            return false;
        }
        output.write(reinterpret_cast<const char*>(headerBytes.data()), headerBytes.size());
        for (const auto* bytes : { &vocabularyBytes, &tileBytes, &histogramBytes, &rowBytes, &postingBytes }) {
            output.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
        }
        output.flush();
        if (!output.good()) {
            error = "failed to write visual index";
            return false;
        }
        if (returnedHeader != nullptr) *returnedHeader = header;
        error.clear();
        return true;
    }
    catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool MapVisualIndexCodec::Load(const std::filesystem::path& path,
    const std::array<std::uint8_t, 32>& expectedImfSha256, std::uint32_t expectedFeatureCount,
    MapVisualIndex& output, std::string& error, MapVisualIndexHeader* returnedHeader) {
    try {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) {
            error = "visual index is missing: " + path.string();
            return false;
        }
        const auto fileSizeValue = input.tellg();
        if (fileSizeValue < static_cast<std::streamoff>(MapVisualIndexHeader::SerializedSize)) {
            error = "visual index is truncated";
            return false;
        }
        const auto fileSize = static_cast<std::uint64_t>(fileSizeValue);
        input.seekg(0);
        std::vector<std::uint8_t> headerBytes(MapVisualIndexHeader::SerializedSize);
        input.read(reinterpret_cast<char*>(headerBytes.data()), headerBytes.size());
        MapVisualIndexHeader header;
        if (input.gcount() != static_cast<std::streamsize>(headerBytes.size()) || !DeserializeHeader(headerBytes, header)) {
            error = "visual index magic or header is invalid";
            return false;
        }
        if (header.version != MapVisualIndexHeader::CurrentVersion ||
            header.headerLength != MapVisualIndexHeader::SerializedSize ||
            header.endianMarker != MapVisualIndexHeader::LittleEndianMarker ||
            header.wordCount != MapVisualIndex::WordCount ||
            header.descriptorColumns != MapVisualIndex::DescriptorColumns ||
            header.tileSize != MapVisualIndex::TileSize || header.tileStride != MapVisualIndex::TileStride ||
            header.featureCount != expectedFeatureCount || header.sourceImfSha256 != expectedImfSha256) {
            error = "visual index version, geometry, feature count, or source IMF hash does not match";
            return false;
        }
        if (header.tileCount == 0 || header.tileCount > kMaximumTiles ||
            header.histogramEntryCount > kMaximumEntries || header.featureRowCount > kMaximumEntries ||
            header.postingCount > kMaximumEntries) {
            error = "visual index counts are outside safe bounds";
            return false;
        }

        std::uint64_t expectedVocabulary = 0, expectedTiles = 0, expectedHistograms = 0;
        std::uint64_t expectedRows = 0, expectedPostings = 0;
        if (!CheckedMultiply<std::uint64_t>(MapVisualIndex::WordCount * MapVisualIndex::DescriptorColumns,
                sizeof(float), expectedVocabulary) ||
            !CheckedMultiply<std::uint64_t>(header.tileCount, kTileSerializedSize, expectedTiles) ||
            !CheckedMultiply<std::uint64_t>(header.histogramEntryCount, kHistogramSerializedSize, expectedHistograms) ||
            !CheckedMultiply<std::uint64_t>(header.featureRowCount, sizeof(std::uint32_t), expectedRows) ||
            !CheckedMultiply<std::uint64_t>(header.postingCount, kPostingSerializedSize, expectedPostings) ||
            expectedVocabulary != header.vocabularyPayloadLength || expectedTiles != header.tilePayloadLength ||
            expectedHistograms != header.histogramPayloadLength || expectedRows != header.featureRowPayloadLength ||
            expectedPostings != header.postingPayloadLength) {
            error = "visual index payload lengths do not match counts";
            return false;
        }
        std::uint64_t expectedFileSize = header.headerLength;
        for (const auto length : { header.vocabularyPayloadLength, header.tilePayloadLength,
                header.histogramPayloadLength, header.featureRowPayloadLength, header.postingPayloadLength }) {
            if (!CheckedAdd(expectedFileSize, length, expectedFileSize)) {
                error = "visual index file length overflow";
                return false;
            }
        }
        if (expectedFileSize != fileSize) {
            error = "visual index file length does not match header";
            return false;
        }

        std::vector<std::uint8_t> payload(static_cast<std::size_t>(fileSize - header.headerLength));
        input.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        if (input.gcount() != static_cast<std::streamsize>(payload.size()) || HashBytes(payload) != header.payloadSha256) {
            error = "visual index payload SHA-256 mismatch";
            return false;
        }

        MapVisualIndex loaded;
        loaded.featureCount = header.featureCount;
        loaded.sourceImfSha256 = header.sourceImfSha256;
        loaded.vocabularySha256 = header.vocabularySha256;
        std::size_t offset = 0;
        const std::size_t vocabularyEnd = offset + static_cast<std::size_t>(header.vocabularyPayloadLength);
        if (HashBytes(std::vector<std::uint8_t>(payload.begin(), payload.begin() + vocabularyEnd)) != header.vocabularySha256) {
            error = "visual index vocabulary SHA-256 mismatch";
            return false;
        }
        loaded.vocabulary.create(MapVisualIndex::WordCount, MapVisualIndex::DescriptorColumns, CV_32FC1);
        for (int row = 0; row < loaded.vocabulary.rows; ++row) {
            auto* values = loaded.vocabulary.ptr<float>(row);
            for (int column = 0; column < loaded.vocabulary.cols; ++column) {
                if (!ReadFloat(payload, offset, values[column])) {
                    error = "visual index vocabulary is invalid";
                    return false;
                }
            }
        }
        loaded.tiles.reserve(header.tileCount);
        for (std::uint32_t index = 0; index < header.tileCount; ++index) {
            MapVisualTile tile;
            if (!ReadLittleEndian(payload, offset, tile.sceneId) ||
                !ReadLittleEndian(payload, offset, tile.gridX) || !ReadLittleEndian(payload, offset, tile.gridY) ||
                !ReadFloat(payload, offset, tile.minX) || !ReadFloat(payload, offset, tile.minY) ||
                !ReadFloat(payload, offset, tile.maxX) || !ReadFloat(payload, offset, tile.maxY) ||
                !ReadLittleEndian(payload, offset, tile.histogramOffset) ||
                !ReadLittleEndian(payload, offset, tile.histogramCount) ||
                !ReadLittleEndian(payload, offset, tile.featureRowOffset) ||
                !ReadLittleEndian(payload, offset, tile.featureRowCount) || !ReadFloat(payload, offset, tile.histogramNorm) ||
                !Scene::IsKnown(tile.sceneId) || tile.minX >= tile.maxX || tile.minY >= tile.maxY ||
                static_cast<std::uint64_t>(tile.histogramOffset) + tile.histogramCount > header.histogramEntryCount ||
                static_cast<std::uint64_t>(tile.featureRowOffset) + tile.featureRowCount > header.featureRowCount) {
                error = "visual index tile is invalid";
                return false;
            }
            loaded.tiles.push_back(tile);
        }
        loaded.histograms.reserve(header.histogramEntryCount);
        for (std::uint32_t index = 0; index < header.histogramEntryCount; ++index) {
            MapVisualHistogramEntry entry;
            if (!ReadLittleEndian(payload, offset, entry.wordId) || !ReadFloat(payload, offset, entry.weight) ||
                entry.wordId >= MapVisualIndex::WordCount || entry.weight <= 0.0f) {
                error = "visual index histogram entry is invalid";
                return false;
            }
            loaded.histograms.push_back(entry);
        }
        loaded.featureRows.reserve(header.featureRowCount);
        for (std::uint32_t index = 0; index < header.featureRowCount; ++index) {
            std::uint32_t row = 0;
            if (!ReadLittleEndian(payload, offset, row) || row >= header.featureCount) {
                error = "visual index feature row is out of range";
                return false;
            }
            loaded.featureRows.push_back(row);
        }
        loaded.postings.reserve(header.postingCount);
        for (std::uint32_t index = 0; index < header.postingCount; ++index) {
            MapVisualPosting posting;
            if (!ReadLittleEndian(payload, offset, posting.wordId) ||
                !ReadLittleEndian(payload, offset, posting.tileIndex) || !ReadFloat(payload, offset, posting.weight)) {
                error = "visual index posting is truncated";
                return false;
            }
            loaded.postings.push_back(posting);
        }
        if (offset != payload.size() || !BuildPostingOffsets(loaded, error)) return false;
        output = std::move(loaded);
        if (returnedHeader != nullptr) *returnedHeader = header;
        error.clear();
        return true;
    }
    catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool MapVisualIndexBuilder::Build(const std::vector<cv::KeyPoint>& keypoints,
    const cv::Mat& inputDescriptors, const std::array<std::uint8_t, 32>& sourceImfSha256,
    MapVisualIndex& output, std::string& error, const cv::Mat& fixedVocabulary,
    int forcedSceneId) {
    try {
        if (!IsValidFeatureInput(keypoints, inputDescriptors, error)) return false;
        if (forcedSceneId < 0 || (forcedSceneId > 0 && !Scene::IsKnown(forcedSceneId))) {
            error = "visual index forced scene ID is invalid";
            return false;
        }
        const cv::Mat descriptors = inputDescriptors.isContinuous() ? inputDescriptors : inputDescriptors.clone();
        cv::Mat vocabulary;
        if (!fixedVocabulary.empty()) {
            if (fixedVocabulary.type() != CV_32FC1 ||
                fixedVocabulary.rows != static_cast<int>(MapVisualIndex::WordCount) ||
                fixedVocabulary.cols != static_cast<int>(MapVisualIndex::DescriptorColumns)) {
                error = "fixed visual vocabulary has the wrong shape or type";
                return false;
            }
            vocabulary = fixedVocabulary.clone();
        }
        else {
            const int sampleCount = std::min(descriptors.rows, 65'536);
            if (sampleCount < static_cast<int>(MapVisualIndex::WordCount)) {
                error = "visual index requires at least 4096 feature descriptors";
                return false;
            }
            cv::Mat sample(sampleCount, descriptors.cols, CV_32FC1);
            for (int index = 0; index < sampleCount; ++index) {
                const auto source = static_cast<int>((static_cast<std::int64_t>(index) * descriptors.rows) / sampleCount);
                descriptors.row(source).copyTo(sample.row(index));
            }

            cvflann::seed_random(0x494d414fU);
            vocabulary.create(MapVisualIndex::WordCount, MapVisualIndex::DescriptorColumns, CV_32FC1);
            const cvflann::KMeansIndexParams parameters(16, 11, cvflann::FLANN_CENTERS_GONZALES, 0.2f);
            const int centers = cv::flann::hierarchicalClustering<cv::flann::L2<float>>(
                sample, vocabulary, parameters);
            if (centers != static_cast<int>(MapVisualIndex::WordCount)) {
                error = "hierarchical clustering did not produce exactly 4096 visual words";
                return false;
            }
        }

        cv::flann::Index vocabularyIndex(vocabulary, cv::flann::KDTreeIndexParams(4));
        cv::Mat wordIds(descriptors.rows, 1, CV_32S);
        cv::Mat wordDistances(descriptors.rows, 1, CV_32F);
        vocabularyIndex.knnSearch(descriptors, wordIds, wordDistances, 1, cv::flann::SearchParams(64));

        std::map<TileKey, TileBuildData> buildTiles;
        std::vector<std::int32_t> featureScenes(keypoints.size());
        for (std::uint32_t row = 0; row < keypoints.size(); ++row) {
            const auto point = keypoints[row].pt;
            if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
                error = "visual index keypoint coordinate is not finite";
                return false;
            }
            const auto scene = forcedSceneId > 0 ? forcedSceneId : NearestScene(point);
            featureScenes[row] = scene;
            const int baseX = FloorGrid(point.x);
            const int baseY = FloorGrid(point.y);
            const auto word = static_cast<std::uint32_t>(wordIds.at<int>(row));
            for (const int gridY : { baseY - 1, baseY }) {
                for (const int gridX : { baseX - 1, baseX }) {
                    const float minX = static_cast<float>(gridX * static_cast<int>(MapVisualIndex::TileStride));
                    const float minY = static_cast<float>(gridY * static_cast<int>(MapVisualIndex::TileStride));
                    if (point.x < minX || point.x >= minX + MapVisualIndex::TileSize ||
                        point.y < minY || point.y >= minY + MapVisualIndex::TileSize) continue;
                    const TileKey key{ scene, gridY, gridX };
                    auto& tile = buildTiles[key];
                    tile.sceneId = scene;
                    tile.gridX = gridX;
                    tile.gridY = gridY;
                    ++tile.wordCounts[word];
                    ++tile.coreFeatureCount;
                }
            }
        }

        for (auto iterator = buildTiles.begin(); iterator != buildTiles.end();) {
            if (iterator->second.coreFeatureCount < 8) iterator = buildTiles.erase(iterator);
            else ++iterator;
        }
        if (buildTiles.empty() || buildTiles.size() > kMaximumTiles) {
            error = "visual index produced no usable tiles or too many tiles";
            return false;
        }

        for (std::uint32_t row = 0; row < keypoints.size(); ++row) {
            const auto point = keypoints[row].pt;
            const auto scene = featureScenes[row];
            const int minGridX = static_cast<int>(std::floor((point.x - MapVisualIndex::TileSize -
                MapVisualIndex::VerificationMargin) / MapVisualIndex::TileStride)) + 1;
            const int maxGridX = static_cast<int>(std::floor((point.x + MapVisualIndex::VerificationMargin) /
                MapVisualIndex::TileStride));
            const int minGridY = static_cast<int>(std::floor((point.y - MapVisualIndex::TileSize -
                MapVisualIndex::VerificationMargin) / MapVisualIndex::TileStride)) + 1;
            const int maxGridY = static_cast<int>(std::floor((point.y + MapVisualIndex::VerificationMargin) /
                MapVisualIndex::TileStride));
            for (int gridY = minGridY; gridY <= maxGridY; ++gridY) {
                for (int gridX = minGridX; gridX <= maxGridX; ++gridX) {
                    const auto found = buildTiles.find(TileKey{ scene, gridY, gridX });
                    if (found != buildTiles.end()) found->second.verificationRows.push_back(row);
                }
            }
        }

        std::vector<std::uint32_t> documentFrequency(MapVisualIndex::WordCount, 0);
        for (const auto& [key, tile] : buildTiles) {
            for (const auto& [word, count] : tile.wordCounts) ++documentFrequency[word];
        }

        MapVisualIndex built;
        built.vocabulary = vocabulary;
        built.sourceImfSha256 = sourceImfSha256;
        built.featureCount = static_cast<std::uint32_t>(keypoints.size());
        built.tiles.reserve(buildTiles.size());
        const double documentCount = static_cast<double>(buildTiles.size());
        for (auto& [key, tileData] : buildTiles) {
            std::sort(tileData.verificationRows.begin(), tileData.verificationRows.end());
            tileData.verificationRows.erase(std::unique(tileData.verificationRows.begin(),
                tileData.verificationRows.end()), tileData.verificationRows.end());

            MapVisualTile tile;
            tile.sceneId = tileData.sceneId;
            tile.gridX = tileData.gridX;
            tile.gridY = tileData.gridY;
            tile.minX = static_cast<float>(tile.gridX * static_cast<int>(MapVisualIndex::TileStride));
            tile.minY = static_cast<float>(tile.gridY * static_cast<int>(MapVisualIndex::TileStride));
            tile.maxX = tile.minX + MapVisualIndex::TileSize;
            tile.maxY = tile.minY + MapVisualIndex::TileSize;
            tile.histogramOffset = static_cast<std::uint32_t>(built.histograms.size());
            tile.featureRowOffset = static_cast<std::uint32_t>(built.featureRows.size());

            double squaredNorm = 0.0;
            std::vector<MapVisualHistogramEntry> localHistogram;
            localHistogram.reserve(tileData.wordCounts.size());
            for (const auto& [word, count] : tileData.wordCounts) {
                const double tf = static_cast<double>(count) / tileData.coreFeatureCount;
                const double idf = std::log((documentCount + 1.0) /
                    (static_cast<double>(documentFrequency[word]) + 1.0)) + 1.0;
                const float weight = static_cast<float>(tf * idf);
                localHistogram.push_back({ word, weight });
                squaredNorm += static_cast<double>(weight) * weight;
            }
            const float norm = static_cast<float>(std::sqrt(squaredNorm));
            if (!(norm > 0.0f)) continue;
            for (auto& entry : localHistogram) {
                entry.weight /= norm;
                built.histograms.push_back(entry);
            }
            tile.histogramCount = static_cast<std::uint32_t>(localHistogram.size());
            tile.histogramNorm = 1.0f;
            built.featureRows.insert(built.featureRows.end(), tileData.verificationRows.begin(),
                tileData.verificationRows.end());
            tile.featureRowCount = static_cast<std::uint32_t>(tileData.verificationRows.size());
            built.tiles.push_back(tile);
        }

        for (std::uint32_t tileIndex = 0; tileIndex < built.tiles.size(); ++tileIndex) {
            const auto& tile = built.tiles[tileIndex];
            for (std::uint32_t offset = 0; offset < tile.histogramCount; ++offset) {
                const auto& entry = built.histograms[tile.histogramOffset + offset];
                built.postings.push_back({ entry.wordId, tileIndex, entry.weight });
            }
        }
        std::sort(built.postings.begin(), built.postings.end(), [](const auto& left, const auto& right) {
            return std::tie(left.wordId, left.tileIndex) < std::tie(right.wordId, right.tileIndex);
        });
        if (!MapVisualIndexCodec::BuildPostingOffsets(built, error)) return false;
        built.vocabularySha256 = HashBytes(SerializeVocabulary(built.vocabulary));
        output = std::move(built);
        error.clear();
        return true;
    }
    catch (const cv::Exception& exception) {
        error = exception.what();
        return false;
    }
    catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}
