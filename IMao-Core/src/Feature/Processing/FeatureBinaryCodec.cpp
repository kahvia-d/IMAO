#include "FeatureBinaryCodec.h"
#include "../../Runtime/TextEncoding.h"

#include <bcrypt.h>
#include <zlib.h>

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

// A v2 header is 124 bytes and a v1 header 116; the version field alone tells the
// reader which one it is looking at, because it is the first field after the magic.
std::uint32_t HeaderSizeForVersion(std::uint32_t version) {
    if (version == FeatureBinaryHeader::LegacyVersion) return FeatureBinaryHeader::SerializedSize;
    if (version == FeatureBinaryHeader::CurrentVersion) return FeatureBinaryHeader::QuantizedSerializedSize;
    return 0;
}

std::vector<std::uint8_t> SerializeHeader(const FeatureBinaryHeader& header) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(FeatureBinaryHeader::QuantizedSerializedSize);
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
    if (header.version >= FeatureBinaryHeader::CurrentVersion) {
        AppendFloat32(bytes, header.descriptorScale);
        AppendFloat32(bytes, header.descriptorMaximum);
        AppendFloat32(bytes, header.descriptorMinimum);
        AppendLittleEndian(bytes, header.descriptorOffset);
        // Padding to the declared header size. A reader locates the hashes at the end
        // of the header, so the two only have to agree with each other.
        while (bytes.size() + header.sourceXmlSha256.size() + header.payloadSha256.size() <
            FeatureBinaryHeader::QuantizedSerializedSize) {
            bytes.push_back(0);
        }
    }
    bytes.insert(bytes.end(), header.sourceXmlSha256.begin(), header.sourceXmlSha256.end());
    bytes.insert(bytes.end(), header.payloadSha256.begin(), header.payloadSha256.end());
    return bytes;
}

bool DeserializeHeader(const std::vector<std::uint8_t>& bytes, FeatureBinaryHeader& header) {
    if (bytes.size() < FeatureBinaryHeader::SerializedSize ||
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
    if (header.version >= FeatureBinaryHeader::CurrentVersion) {
        if (!ReadFloat32(bytes, offset, header.descriptorScale) ||
            !ReadFloat32(bytes, offset, header.descriptorMaximum) ||
            !ReadFloat32(bytes, offset, header.descriptorMinimum) ||
            !ReadLittleEndian(bytes, offset, header.descriptorOffset)) return false;
    }
    // The hashes sit at the end of the header, so any bytes between the v2 fields
    // and them are padding the writer added to reach the declared header size.
    const auto hashStart = bytes.size() - header.sourceXmlSha256.size() - header.payloadSha256.size();
    if (hashStart < offset) return false;
    offset = hashStart;
    std::copy_n(bytes.begin() + offset, header.sourceXmlSha256.size(), header.sourceXmlSha256.begin());
    offset += header.sourceXmlSha256.size();
    std::copy_n(bytes.begin() + offset, header.payloadSha256.size(), header.payloadSha256.begin());
    return true;
}

// The payloads are tens to hundreds of megabytes and the caller already holds the
// whole block, so both directions work on one buffer instead of streaming.
bool Deflate(const std::uint8_t* source, std::size_t sourceBytes, std::vector<std::uint8_t>& output) {
    if (sourceBytes == 0) {
        output.clear();
        return true;
    }
    uLongf bound = compressBound(static_cast<uLong>(sourceBytes));
    output.resize(bound);
    const auto result = compress2(output.data(), &bound, source, static_cast<uLong>(sourceBytes), 6);
    if (result != Z_OK) return false;
    output.resize(bound);
    return true;
}

bool Inflate(const std::uint8_t* source, std::size_t sourceBytes, std::size_t expectedBytes,
    std::vector<std::uint8_t>& output) {
    output.resize(expectedBytes);
    if (expectedBytes == 0) return true;
    uLongf produced = static_cast<uLongf>(expectedBytes);
    const auto result = uncompress(output.data(), &produced, source, static_cast<uLong>(sourceBytes));
    return result == Z_OK && produced == expectedBytes;
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
            error = "feature binary cannot be opened: " + Utf8Text(path);
            return false;
        }

        // The version decides how long the header is, so the magic and version are
        // read as a fixed 12-byte prefix and the remainder follows once it is known.
        std::vector<std::uint8_t> headerBytes(FeatureBinaryHeader::Magic.size() + sizeof(std::uint32_t));
        if (!ReadExact(input, headerBytes.data(), headerBytes.size())) {
            error = "feature binary header is truncated";
            return false;
        }
        std::uint32_t declaredVersion = 0;
        {
            std::size_t prefixOffset = FeatureBinaryHeader::Magic.size();
            if (!std::equal(FeatureBinaryHeader::Magic.begin(), FeatureBinaryHeader::Magic.end(),
                    headerBytes.begin()) ||
                !ReadLittleEndian(headerBytes, prefixOffset, declaredVersion)) {
                error = "feature binary magic or header encoding is invalid";
                return false;
            }
        }
        const auto headerSize = HeaderSizeForVersion(declaredVersion);
        if (headerSize == 0) {
            error = "feature binary version is unsupported";
            return false;
        }
        const auto remainingHeaderBytes = static_cast<std::size_t>(headerSize) - headerBytes.size();
        headerBytes.resize(headerSize);
        if (!ReadExact(input, headerBytes.data() + FeatureBinaryHeader::Magic.size() + sizeof(std::uint32_t),
                remainingHeaderBytes)) {
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
        const bool quantized = header.version >= FeatureBinaryHeader::CurrentVersion;
        const auto expectedDescriptorType = quantized
            ? FeatureBinaryHeader::QuantizedUint8DescriptorType
            : FeatureBinaryHeader::Float32DescriptorType;
        if (header.headerLength != headerSize ||
            header.endianMarker != FeatureBinaryHeader::LittleEndianMarker ||
            header.keypointCount == 0 || header.keypointCount > kMaximumKeypointCount ||
            header.descriptorRows != header.keypointCount || header.descriptorColumns != 128 ||
            header.descriptorType != expectedDescriptorType) {
            error = "feature binary counts or descriptor shape are invalid";
            return false;
        }
        if (quantized && (!std::isfinite(header.descriptorScale) || !std::isfinite(header.descriptorMaximum) ||
            !std::isfinite(header.descriptorMinimum) ||
            header.descriptorMaximum < header.descriptorMinimum ||
            header.descriptorScale <= 0.0f ||
            header.descriptorScale > FeatureBinaryHeader::MaximumQuantizationScale)) {
            error = "feature binary quantization range is invalid";
            return false;
        }

        std::uint64_t expectedKeypointBytes = 0;
        std::uint64_t expectedDescriptorValues = 0;
        std::uint64_t expectedDescriptorBytes = 0;
        if (!CheckedMultiply(header.keypointCount, FeatureBinaryHeader::KeypointSerializedSize, expectedKeypointBytes) ||
            !CheckedMultiply(header.descriptorRows, header.descriptorColumns, expectedDescriptorValues) ||
            !CheckedMultiply(expectedDescriptorValues, sizeof(float), expectedDescriptorBytes)) {
            error = "feature binary payload lengths overflow";
            return false;
        }
        // v1 stores both payloads raw, so each length is exact. v2 deflates both, so
        // each stored length only has to be a plausible deflate result for the block
        // it restores; the deflate bound is what makes that check possible without
        // trusting the header. The header also cannot claim more rows than a matrix
        // that large could hold, which bounds the allocation below.
        const bool keypointLengthValid = quantized
            ? header.keypointPayloadLength > 0 &&
                header.keypointPayloadLength <= compressBound(static_cast<uLong>(expectedKeypointBytes))
            : header.keypointPayloadLength == expectedKeypointBytes;
        const bool descriptorLengthValid = quantized
            ? header.descriptorPayloadLength > 0 &&
                header.descriptorPayloadLength <= compressBound(static_cast<uLong>(expectedDescriptorValues))
            : header.descriptorPayloadLength == expectedDescriptorBytes;
        if (!keypointLengthValid || !descriptorLengthValid) {
            error = "feature binary payload lengths overflow or do not match the header";
            return false;
        }

        const auto actualFileSize = std::filesystem::file_size(path);
        const std::uint64_t expectedFileSize = header.headerLength + header.keypointPayloadLength + header.descriptorPayloadLength;
        if (actualFileSize != expectedFileSize) {
            error = "feature binary file length does not match its header";
            return false;
        }

        // The payload hash covers what the file restores - the serialized keypoints
        // and the uint8 descriptor codes - and not the compressed bytes, because that
        // is what Save hashed. Each block is therefore hashed after it is inflated.
        Sha256State payloadHash;
        std::vector<std::uint8_t> storedKeypoints(static_cast<std::size_t>(header.keypointPayloadLength));
        if (!ReadExact(input, storedKeypoints.data(), header.keypointPayloadLength)) {
            error = "feature keypoint payload is truncated";
            return false;
        }
        fileHash.Update(storedKeypoints.data(), storedKeypoints.size());
        std::vector<std::uint8_t> keypointBytes;
        if (quantized) {
            if (!Inflate(storedKeypoints.data(), storedKeypoints.size(),
                    static_cast<std::size_t>(expectedKeypointBytes), keypointBytes)) {
                error = "feature keypoint payload cannot be inflated";
                return false;
            }
        }
        else {
            keypointBytes = std::move(storedKeypoints);
        }
        payloadHash.Update(keypointBytes.data(), keypointBytes.size());

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
        if (!output.imgDescriptors.isContinuous()) {
            output.Release();
            error = "feature descriptor storage is non-contiguous";
            return false;
        }
        if (!quantized) {
            if (!ReadExact(input, output.imgDescriptors.data, header.descriptorPayloadLength)) {
                output.Release();
                error = "feature descriptor payload is truncated";
                return false;
            }
            fileHash.Update(output.imgDescriptors.data,
                static_cast<std::size_t>(header.descriptorPayloadLength));
            payloadHash.Update(output.imgDescriptors.data,
                static_cast<std::size_t>(header.descriptorPayloadLength));
        }
        else {
            std::vector<std::uint8_t> storedDescriptors(static_cast<std::size_t>(header.descriptorPayloadLength));
            if (!ReadExact(input, storedDescriptors.data(), header.descriptorPayloadLength)) {
                output.Release();
                error = "feature descriptor payload is truncated";
                return false;
            }
            fileHash.Update(storedDescriptors.data(), storedDescriptors.size());
            std::vector<std::uint8_t> codes;
            const auto codeBytes = static_cast<std::size_t>(expectedDescriptorValues);
            if (!Inflate(storedDescriptors.data(), storedDescriptors.size(), codeBytes, codes)) {
                output.Release();
                error = "feature descriptor payload cannot be inflated";
                return false;
            }
            payloadHash.Update(codes.data(), codes.size());
            auto* values = output.imgDescriptors.ptr<float>();
            const auto offset = static_cast<int>(header.descriptorOffset);
            for (std::size_t index = 0; index < codeBytes; ++index) {
                values[index] = static_cast<float>(static_cast<int>(codes[index]) + offset) * header.descriptorScale;
            }
        }
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
        const auto valueCount = static_cast<std::size_t>(descriptors.total());
        const auto* values = descriptors.ptr<float>();

        // One scale for the whole file, taken from the range of values it holds.
        // Descriptor values are signed, so the range is symmetric about zero and the
        // code carries an offset: encoding is round(value / scale) + offset, which
        // keeps the step proportional to the pack's own spread (SURF's extended
        // descriptors are L2-normalized and land near +/-0.6, SIFT's gradients land
        // in the hundreds).
        float largest = 0.0f;
        float smallest = 0.0f;
        for (std::size_t index = 0; index < valueCount; ++index) {
            const auto value = values[index];
            if (value > largest) largest = value;
            if (value < smallest) smallest = value;
        }
        if (!std::isfinite(largest) || !std::isfinite(smallest)) {
            error = "input descriptors are not finite";
            return false;
        }
        // Codes run from 0 to MaximumQuantizationOffset, so dividing the span by that
        // offset (rather than by offset + 1) puts the largest value on the last code
        // instead of one past it, which is what keeps the half-step bound honest.
        const auto span = largest - smallest;
        const auto codeCount = static_cast<float>(FeatureBinaryHeader::MaximumQuantizationOffset);
        const auto scale = span > 0.0f ? span / codeCount : 0.0f;
        if (!std::isfinite(scale) || (span > 0.0f && scale > FeatureBinaryHeader::MaximumQuantizationScale)) {
            error = "input descriptor range is too wide to quantize";
            return false;
        }
        // The offset is the code the smallest value maps to, so that the whole range
        // lands inside 0..255 instead of being clamped at one end. Storing a value is
        // then round(value / scale) - offset, and restoring it is
        // (code + offset) * scale.
        const auto offsetValue = span > 0.0f
            ? static_cast<int>(std::lround(static_cast<double>(smallest) / scale))
            : 0;
        std::vector<std::uint8_t> codes(valueCount);
        if (scale > 0.0f) {
            for (std::size_t index = 0; index < valueCount; ++index) {
                const auto code = std::lround(static_cast<double>(values[index]) / scale) - offsetValue;
                codes[index] = static_cast<std::uint8_t>(code < 0 ? 0 : (code > 255 ? 255 : code));
            }
        }

        std::vector<std::uint8_t> deflatedKeypoints;
        std::vector<std::uint8_t> deflatedDescriptors;
        if (!Deflate(keypointBytes.data(), keypointBytes.size(), deflatedKeypoints) ||
            !Deflate(codes.data(), codes.size(), deflatedDescriptors)) {
            error = "feature payload compression failed";
            return false;
        }

        // The hash covers the serialized keypoints and the uint8 codes - what the
        // file restores - rather than the compressed bytes, so the same features
        // hash the same way whatever the deflate settings produce.
        Sha256State payloadHash;
        payloadHash.Update(keypointBytes.data(), keypointBytes.size());
        payloadHash.Update(codes.data(), codes.size());

        FeatureBinaryHeader header;
        header.version = FeatureBinaryHeader::CurrentVersion;
        header.headerLength = FeatureBinaryHeader::QuantizedSerializedSize;
        header.descriptorType = FeatureBinaryHeader::QuantizedUint8DescriptorType;
        header.keypointCount = static_cast<std::uint32_t>(input.imgKeypoints.size());
        header.descriptorRows = static_cast<std::uint32_t>(descriptors.rows);
        header.descriptorColumns = static_cast<std::uint32_t>(descriptors.cols);
        header.keypointPayloadLength = deflatedKeypoints.size();
        header.descriptorPayloadLength = deflatedDescriptors.size();
        header.descriptorScale = scale;
        header.descriptorMaximum = largest;
        header.descriptorMinimum = smallest;
        header.descriptorOffset = offsetValue;
        header.sourceXmlSha256 = sourceXmlSha256;
        header.payloadSha256 = payloadHash.Finish();
        const auto headerBytes = SerializeHeader(header);

        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output || !WriteExact(output, headerBytes.data(), headerBytes.size()) ||
            !WriteExact(output, deflatedKeypoints.data(), deflatedKeypoints.size()) ||
            !WriteExact(output, deflatedDescriptors.data(), deflatedDescriptors.size())) {
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

float FeatureBinaryCodec::QuantizationTolerance(const FeatureBinaryHeader& header) {
    if (header.version < FeatureBinaryHeader::CurrentVersion || header.descriptorScale <= 0.0f) return 0.0f;
    // Half a code is the bound for reading a value back; the extra part covers the
    // float rounding in the two divisions that encode and decode it, which for a SIFT
    // range near 7000 is about a thousandth of a code.
    return header.descriptorScale * 0.501f;
}

bool FeatureBinaryCodec::SaveLegacyFloat32(const std::filesystem::path& path, const ImageFeatureData& input,
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
        header.version = FeatureBinaryHeader::LegacyVersion;
        header.headerLength = FeatureBinaryHeader::SerializedSize;
        header.descriptorType = FeatureBinaryHeader::Float32DescriptorType;
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
            error = "file cannot be opened for SHA-256: " + Utf8Text(path);
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
            error = "text source cannot be opened: " + Utf8Text(path);
            return false;
        }
        // Candidate manifests are small; bound allocation for invalid inputs.
        constexpr std::size_t maxManifestBytes = 4 * 1024 * 1024;
        std::vector<std::uint8_t> bytes;
        char value;
        while (input.get(value)) {
            if (bytes.size() >= maxManifestBytes) {
                error = "text source exceeds manifest size limit: " + Utf8Text(path);
                return false;
            }
            if (value == '\n' && !bytes.empty() && bytes.back() == '\r') bytes.pop_back();
            bytes.push_back(static_cast<std::uint8_t>(value));
        }
        if (!input.eof()) {
            error = "text source read failed: " + Utf8Text(path);
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
