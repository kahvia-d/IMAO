#pragma once

#include "../Match/FeatureMatch.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

struct FeatureBinaryHeader {
    static constexpr std::array<char, 8> Magic = { 'I', 'M', 'A', 'O', 'F', 'T', '0', '1' };
    // Version 1 stores 128 raw float32 descriptors, uncompressed: 540 bytes per
    // keypoint. Every pack shipped before 2026-10-02 is v1 and must keep loading.
    static constexpr std::uint32_t LegacyVersion = 1;
    // Version 2 stores the same descriptors as 128 uint8 codes against one per-file
    // scale and offset, with both payloads deflated: ~110 bytes per keypoint for a
    // SURF pack. Storing a code is round(value / descriptorScale + descriptorOffset)
    // and restoring it is (code - descriptorOffset) * descriptorScale. The offset
    // matters because descriptor values are signed: without it the negative half of
    // every row would clamp to zero.
    static constexpr std::uint32_t CurrentVersion = 2;
    static constexpr std::uint32_t LittleEndianMarker = 0x01020304;
    static constexpr std::uint32_t Float32DescriptorType = 1;
    static constexpr std::uint32_t QuantizedUint8DescriptorType = 2;
    static constexpr std::uint32_t SerializedSize = 116;          // v1
    static constexpr std::uint32_t QuantizedSerializedSize = 136; // v2
    static constexpr std::uint32_t KeypointSerializedSize = 28;
    // v2 refuses to quantize finer than this: a scale above it would mean the
    // descriptors carry values no real detector produces.
    static constexpr float MaximumQuantizationScale = 4096.0f;
    static constexpr std::uint8_t MaximumQuantizationOffset = 255;

    std::uint32_t version = CurrentVersion;
    std::uint32_t headerLength = QuantizedSerializedSize;
    std::uint32_t endianMarker = LittleEndianMarker;
    std::uint32_t keypointCount = 0;
    std::uint32_t descriptorRows = 0;
    std::uint32_t descriptorColumns = 0;
    std::uint32_t descriptorType = QuantizedUint8DescriptorType;
    // Stored, on-disk payload bytes: deflated for v2, raw for v1.
    std::uint64_t keypointPayloadLength = 0;
    std::uint64_t descriptorPayloadLength = 0;
    // v2 only: the code range the file was quantized against. The offset is signed
    // because it is the code the smallest value maps to, and descriptor values are
    // signed.
    float descriptorScale = 0.0f;
    float descriptorMaximum = 0.0f;
    float descriptorMinimum = 0.0f;
    std::int32_t descriptorOffset = 0;
    std::array<std::uint8_t, 32> sourceXmlSha256{};
    std::array<std::uint8_t, 32> payloadSha256{};
};

class FeatureBinaryCodec {
public:
    static bool Load(const std::filesystem::path& path, ImageFeatureData& output,
        std::string& error, FeatureBinaryHeader* header = nullptr,
        std::array<std::uint8_t, 32>* fileSha256 = nullptr);

    // Writes the codec's current version (v2: quantized descriptors, deflated
    // payloads). `input.imgDescriptors` stays CV_32FC1 in memory; only the file
    // representation is quantized, so nothing downstream changes.
    static bool Save(const std::filesystem::path& path, const ImageFeatureData& input,
        const std::array<std::uint8_t, 32>& sourceXmlSha256, std::string& error,
        FeatureBinaryHeader* header = nullptr);

    // Writes the legacy uncompressed float32 representation. It exists so a pack
    // built by an older tool can be reproduced byte for byte, not as a fallback.
    static bool SaveLegacyFloat32(const std::filesystem::path& path, const ImageFeatureData& input,
        const std::array<std::uint8_t, 32>& sourceXmlSha256, std::string& error,
        FeatureBinaryHeader* header = nullptr);

    // Largest descriptor deviation an exact-looking v2 round trip can produce:
    // one quantization step. A comparator that checks v2 output must allow this.
    static float QuantizationTolerance(const FeatureBinaryHeader& header);

    static bool Sha256File(const std::filesystem::path& path,
        std::array<std::uint8_t, 32>& output, std::string& error);

    // Only CRLF is normalized. All other bytes remain part of the source hash.
    static bool Sha256LfTextFile(const std::filesystem::path& path,
        std::array<std::uint8_t, 32>& output, std::string& error);

    static std::string Sha256Hex(const std::array<std::uint8_t, 32>& hash);
};
