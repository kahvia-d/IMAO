#include "FeatureBinaryCodec.h"

#include <bcrypt.h>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <type_traits>
#include <vector>

namespace {
constexpr std::uint64_t kMaximumKeypointCount = 10'000'000;
constexpr std::size_t kHashChunkSize = 1024 * 1024;

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
                reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &received, 0)) ||
            hashLength != 32) {
            throw std::runtime_error("SHA-256 provider initialization failed");
        }
        hashObject_.resize(objectLength);
        if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm_, &hash_, hashObject_.data(), objectLength, nullptr, 0, 0))) {
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
            const ULONG chunk = static_cast<ULONG>(std::min<std::size_t>(size, std::numeric_limits<ULONG>::max()));
            if (!BCRYPT_SUCCESS(BCryptHashData(hash_, const_cast<PUCHAR>(bytes), chunk, 0))) {
                throw std::runtime_error("SHA-256 update failed");
            }
            bytes += chunk;
            size -= chunk;
        }
    }

    std::array<std::uint8_t, 32> Finish() {
        std::array<std::uint8_t, 32> result{};
        if (!BCRYPT_SUCCESS(BCryptFinishHash(hash_, result.data(), static_cast<ULONG>(result.size()), 0))) {
            throw std::runtime_error("SHA-256 finalization failed");
        }
        BCryptDestroyHash(hash_);
        hash_ = nullptr;
        return result;
    }

private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    std::vector<std::uint8_t> hashObject_;
};

template <typename T>
void AppendLittleEndian(std::vector<std::uint8_t>& output, T value) {
    static_assert(std::is_integral_v<T>);
    using Unsigned = std::make_unsigned_t<T>;
    const Unsigned bits = static_cast<Unsigned>(value);
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        output.push_back(static_cast<std::uint8_t>((bits >> (index * 8)) & 0xff));
    }
}

void AppendFloat32(std::vector<std::uint8_t>& output, float value) {
    static_assert(sizeof(float) == sizeof(std::uint32_t));
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

bool ReadFloat32(const std::vector<std::uint8_t>& input, std::size_t& offset, float& output) {
    std::uint32_t bits = 0;
    if (!ReadLittleEndian(input, offset, bits)) return false;
    std::memcpy(&output, &bits, sizeof(output));
    return true;
}

bool CheckedMultiply(std::uint64_t first, std::uint64_t second, std::uint64_t& result) {
    if (first != 0 && second > std::numeric_limits<std::uint64_t>::max() / first) return false;
    result = first * second;
    return true;
}

bool ReadExact(std::ifstream& input, void* destination, std::uint64_t byteCount, Sha256State* hash = nullptr) {
    auto* bytes = static_cast<std::uint8_t*>(destination);
    while (byteCount > 0) {
        const auto chunk = static_cast<std::streamsize>(std::min<std::uint64_t>(byteCount, kHashChunkSize));
        input.read(reinterpret_cast<char*>(bytes), chunk);
        if (input.gcount() != chunk) return false;
        if (hash != nullptr) hash->Update(bytes, static_cast<std::size_t>(chunk));
        bytes += chunk;
        byteCount -= static_cast<std::uint64_t>(chunk);
    }
    return true;
}

std::vector<std::uint8_t> SerializeHeader(const FeatureBinaryHeader& header) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(FeatureBinaryHeader::SerializedSize);
    bytes.insert(bytes.end(), FeatureBinaryHeader::Magic.begin(), FeatureBinaryHeader::Magic.end());
    AppendLittleEndian(bytes, header.version);
    AppendLittleEndian(bytes, header.headerLength);
    AppendLittleEndian(bytes, header.endianMarker);
    AppendLittleEndian(bytes, header.keypointCount);
    AppendLittleEndian(bytes, header.descriptorRows);
    AppendLittleEndian(bytes, header.descriptorColumns);
    AppendLittleEndian(bytes, header.descriptorType);
    AppendLittleEndian(bytes, header.keypointPayloadLength);
    AppendLittleEndian(bytes, header.descriptorPayloadLength);
    bytes.insert(bytes.end(), header.sourceXmlSha256.begin(), header.sourceXmlSha256.end());
    bytes.insert(bytes.end(), header.payloadSha256.begin(), header.payloadSha256.end());
    return bytes;
}

bool DeserializeHeader(const std::vector<std::uint8_t>& bytes, FeatureBinaryHeader& header) {
    if (bytes.size() != FeatureBinaryHeader::SerializedSize ||
        !std::equal(FeatureBinaryHeader::Magic.begin(), FeatureBinaryHeader::Magic.end(), bytes.begin())) return false;
    std::size_t offset = FeatureBinaryHeader::Magic.size();
    if (!ReadLittleEndian(bytes, offset, header.version) ||
        !ReadLittleEndian(bytes, offset, header.headerLength) ||
        !ReadLittleEndian(bytes, offset, header.endianMarker) ||
        !ReadLittleEndian(bytes, offset, header.keypointCount) ||
        !ReadLittleEndian(bytes, offset, header.descriptorRows) ||
        !ReadLittleEndian(bytes, offset, header.descriptorColumns) ||
        !ReadLittleEndian(bytes, offset, header.descriptorType) ||
        !ReadLittleEndian(bytes, offset, header.keypointPayloadLength) ||
        !ReadLittleEndian(bytes, offset, header.descriptorPayloadLength)) return false;
    std::copy_n(bytes.begin() + offset, header.sourceXmlSha256.size(), header.sourceXmlSha256.begin());
    offset += header.sourceXmlSha256.size();
    std::copy_n(bytes.begin() + offset, header.payloadSha256.size(), header.payloadSha256.begin());
    return true;
}

std::vector<std::uint8_t> SerializeKeypoints(const std::vector<cv::KeyPoint>& keypoints) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(keypoints.size() * FeatureBinaryHeader::KeypointSerializedSize);
    for (const auto& keypoint : keypoints) {
        AppendFloat32(bytes, keypoint.pt.x);
        AppendFloat32(bytes, keypoint.pt.y);
        AppendFloat32(bytes, keypoint.size);
        AppendFloat32(bytes, keypoint.angle);
        AppendFloat32(bytes, keypoint.response);
        AppendLittleEndian(bytes, static_cast<std::int32_t>(keypoint.octave));
        AppendLittleEndian(bytes, static_cast<std::int32_t>(keypoint.class_id));
    }
    return bytes;
}

bool WriteExact(std::ofstream& output, const void* data, std::uint64_t byteCount) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    while (byteCount > 0) {
        const auto chunk = static_cast<std::streamsize>(std::min<std::uint64_t>(byteCount, kHashChunkSize));
        output.write(reinterpret_cast<const char*>(bytes), chunk);
        if (!output) return false;
        bytes += chunk;
        byteCount -= static_cast<std::uint64_t>(chunk);
    }
    return true;
}
}

bool FeatureBinaryCodec::Load(const std::filesystem::path& path, ImageFeatureData& output,
    std::string& error, FeatureBinaryHeader* returnedHeader,
    std::array<std::uint8_t, 32>* returnedFileSha256) {
    output.Release();
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            error = "feature binary cannot be opened: " + path.string();
            return false;
        }

        std::vector<std::uint8_t> headerBytes(FeatureBinaryHeader::SerializedSize);
        if (!ReadExact(input, headerBytes.data(), headerBytes.size())) {
            error = "feature binary header is truncated";
            return false;
        }
        Sha256State fileHash;
        fileHash.Update(headerBytes.data(), headerBytes.size());
        FeatureBinaryHeader header;
        if (!DeserializeHeader(headerBytes, header)) {
            error = "feature binary magic or header encoding is invalid";
            return false;
        }
        if (header.version != FeatureBinaryHeader::CurrentVersion ||
            header.headerLength != FeatureBinaryHeader::SerializedSize ||
            header.endianMarker != FeatureBinaryHeader::LittleEndianMarker) {
            error = "feature binary version, header length, or byte order is unsupported";
            return false;
        }
        if (header.keypointCount == 0 || header.keypointCount > kMaximumKeypointCount ||
            header.descriptorRows != header.keypointCount || header.descriptorColumns != 128 ||
            header.descriptorType != FeatureBinaryHeader::Float32DescriptorType) {
            error = "feature binary counts or descriptor shape are invalid";
            return false;
        }

        std::uint64_t expectedKeypointBytes = 0;
        std::uint64_t expectedDescriptorValues = 0;
        std::uint64_t expectedDescriptorBytes = 0;
        if (!CheckedMultiply(header.keypointCount, FeatureBinaryHeader::KeypointSerializedSize, expectedKeypointBytes) ||
            !CheckedMultiply(header.descriptorRows, header.descriptorColumns, expectedDescriptorValues) ||
            !CheckedMultiply(expectedDescriptorValues, sizeof(float), expectedDescriptorBytes) ||
            header.keypointPayloadLength != expectedKeypointBytes ||
            header.descriptorPayloadLength != expectedDescriptorBytes) {
            error = "feature binary payload lengths overflow or do not match the header";
            return false;
        }

        const auto actualFileSize = std::filesystem::file_size(path);
        const std::uint64_t expectedFileSize = header.headerLength + header.keypointPayloadLength + header.descriptorPayloadLength;
        if (actualFileSize != expectedFileSize) {
            error = "feature binary file length does not match its header";
            return false;
        }

        Sha256State payloadHash;
        std::vector<std::uint8_t> keypointBytes(static_cast<std::size_t>(header.keypointPayloadLength));
        if (!ReadExact(input, keypointBytes.data(), header.keypointPayloadLength, &payloadHash)) {
            error = "feature keypoint payload is truncated";
            return false;
        }
        fileHash.Update(keypointBytes.data(), keypointBytes.size());

        output.imgKeypoints.reserve(header.keypointCount);
        std::size_t offset = 0;
        for (std::uint32_t index = 0; index < header.keypointCount; ++index) {
            cv::KeyPoint keypoint;
            std::int32_t octave = 0;
            std::int32_t classId = 0;
            if (!ReadFloat32(keypointBytes, offset, keypoint.pt.x) ||
                !ReadFloat32(keypointBytes, offset, keypoint.pt.y) ||
                !ReadFloat32(keypointBytes, offset, keypoint.size) ||
                !ReadFloat32(keypointBytes, offset, keypoint.angle) ||
                !ReadFloat32(keypointBytes, offset, keypoint.response) ||
                !ReadLittleEndian(keypointBytes, offset, octave) ||
                !ReadLittleEndian(keypointBytes, offset, classId) ||
                !std::isfinite(keypoint.pt.x) || !std::isfinite(keypoint.pt.y) ||
                !std::isfinite(keypoint.size) || !std::isfinite(keypoint.angle) ||
                !std::isfinite(keypoint.response)) {
                output.Release();
                error = "feature keypoint payload is invalid";
                return false;
            }
            keypoint.octave = octave;
            keypoint.class_id = classId;
            output.imgKeypoints.push_back(keypoint);
        }

        output.imgDescriptors.create(static_cast<int>(header.descriptorRows),
            static_cast<int>(header.descriptorColumns), CV_32FC1);
        if (!output.imgDescriptors.isContinuous() ||
            !ReadExact(input, output.imgDescriptors.data, header.descriptorPayloadLength, &payloadHash)) {
            output.Release();
            error = "feature descriptor payload is truncated or non-contiguous";
            return false;
        }
        fileHash.Update(output.imgDescriptors.data,
            static_cast<std::size_t>(header.descriptorPayloadLength));
        if (payloadHash.Finish() != header.payloadSha256) {
            output.Release();
            error = "feature payload SHA-256 mismatch";
            return false;
        }

        if (returnedHeader != nullptr) *returnedHeader = header;
        if (returnedFileSha256 != nullptr) *returnedFileSha256 = fileHash.Finish();
        error.clear();
        return true;
    }
    catch (const std::exception& exception) {
        output.Release();
        error = exception.what();
        return false;
    }
}

bool FeatureBinaryCodec::Save(const std::filesystem::path& path, const ImageFeatureData& input,
    const std::array<std::uint8_t, 32>& sourceXmlSha256, std::string& error,
    FeatureBinaryHeader* returnedHeader) {
    try {
        if (input.imgKeypoints.empty() || input.imgKeypoints.size() > kMaximumKeypointCount ||
            input.imgDescriptors.empty() || input.imgDescriptors.type() != CV_32FC1 ||
            input.imgDescriptors.cols != 128 ||
            input.imgDescriptors.rows != static_cast<int>(input.imgKeypoints.size())) {
            error = "input feature counts or descriptor shape are invalid";
            return false;
        }

        cv::Mat descriptors = input.imgDescriptors.isContinuous()
            ? input.imgDescriptors
            : input.imgDescriptors.clone();
        const auto keypointBytes = SerializeKeypoints(input.imgKeypoints);
        const std::uint64_t descriptorBytes = descriptors.total() * descriptors.elemSize();

        Sha256State payloadHash;
        payloadHash.Update(keypointBytes.data(), keypointBytes.size());
        payloadHash.Update(descriptors.data, static_cast<std::size_t>(descriptorBytes));

        FeatureBinaryHeader header;
        header.keypointCount = static_cast<std::uint32_t>(input.imgKeypoints.size());
        header.descriptorRows = static_cast<std::uint32_t>(descriptors.rows);
        header.descriptorColumns = static_cast<std::uint32_t>(descriptors.cols);
        header.keypointPayloadLength = keypointBytes.size();
        header.descriptorPayloadLength = descriptorBytes;
        header.sourceXmlSha256 = sourceXmlSha256;
        header.payloadSha256 = payloadHash.Finish();
        const auto headerBytes = SerializeHeader(header);

        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output || !WriteExact(output, headerBytes.data(), headerBytes.size()) ||
            !WriteExact(output, keypointBytes.data(), keypointBytes.size()) ||
            !WriteExact(output, descriptors.data, descriptorBytes)) {
            error = "feature binary write failed";
            return false;
        }
        output.flush();
        if (!output) {
            error = "feature binary flush failed";
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

bool FeatureBinaryCodec::Sha256File(const std::filesystem::path& path,
    std::array<std::uint8_t, 32>& output, std::string& error) {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            error = "file cannot be opened for SHA-256: " + path.string();
            return false;
        }
        Sha256State hash;
        std::vector<std::uint8_t> buffer(kHashChunkSize);
        while (input) {
            input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            const auto read = input.gcount();
            if (read > 0) hash.Update(buffer.data(), static_cast<std::size_t>(read));
        }
        if (!input.eof()) {
            error = "file read failed while calculating SHA-256";
            return false;
        }
        output = hash.Finish();
        error.clear();
        return true;
    }
    catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool FeatureBinaryCodec::Sha256LfTextFile(const std::filesystem::path& path,
    std::array<std::uint8_t, 32>& output, std::string& error) {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            error = "text source cannot be opened: " + path.string();
            return false;
        }
        // Candidate manifests are small; bound allocation for invalid inputs.
        constexpr std::size_t maxManifestBytes = 4 * 1024 * 1024;
        std::vector<std::uint8_t> bytes;
        char value;
        while (input.get(value)) {
            if (bytes.size() >= maxManifestBytes) {
                error = "text source exceeds manifest size limit: " + path.string();
                return false;
            }
            if (value == '\n' && !bytes.empty() && bytes.back() == '\r') bytes.pop_back();
            bytes.push_back(static_cast<std::uint8_t>(value));
        }
        if (!input.eof()) {
            error = "text source read failed: " + path.string();
            return false;
        }
        Sha256State hash;
        hash.Update(bytes.data(), bytes.size());
        output = hash.Finish();
        error.clear();
        return true;
    }
    catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

std::string FeatureBinaryCodec::Sha256Hex(const std::array<std::uint8_t, 32>& hash) {
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (const auto byte : hash) result << std::setw(2) << static_cast<int>(byte);
    return result.str();
}
