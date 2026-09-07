#pragma once

#include "../Match/FeatureMatch.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

struct FeatureBinaryHeader {
    static constexpr std::array<char, 8> Magic = { 'I', 'M', 'A', 'O', 'F', 'T', '0', '1' };
    static constexpr std::uint32_t CurrentVersion = 1;
    static constexpr std::uint32_t LittleEndianMarker = 0x01020304;
    static constexpr std::uint32_t Float32DescriptorType = 1;
    static constexpr std::uint32_t SerializedSize = 116;
    static constexpr std::uint32_t KeypointSerializedSize = 28;

    std::uint32_t version = CurrentVersion;
    std::uint32_t headerLength = SerializedSize;
    std::uint32_t endianMarker = LittleEndianMarker;
    std::uint32_t keypointCount = 0;
    std::uint32_t descriptorRows = 0;
    std::uint32_t descriptorColumns = 0;
    std::uint32_t descriptorType = Float32DescriptorType;
    std::uint64_t keypointPayloadLength = 0;
    std::uint64_t descriptorPayloadLength = 0;
    std::array<std::uint8_t, 32> sourceXmlSha256{};
    std::array<std::uint8_t, 32> payloadSha256{};
};

class FeatureBinaryCodec {
public:
    static bool Load(const std::filesystem::path& path, ImageFeatureData& output,
        std::string& error, FeatureBinaryHeader* header = nullptr,
        std::array<std::uint8_t, 32>* fileSha256 = nullptr);

    static bool Save(const std::filesystem::path& path, const ImageFeatureData& input,
        const std::array<std::uint8_t, 32>& sourceXmlSha256, std::string& error,
        FeatureBinaryHeader* header = nullptr);

    static bool Sha256File(const std::filesystem::path& path,
        std::array<std::uint8_t, 32>& output, std::string& error);

    // Only CRLF is normalized. All other bytes remain part of the source hash.
    static bool Sha256LfTextFile(const std::filesystem::path& path,
        std::array<std::uint8_t, 32>& output, std::string& error);

    static std::string Sha256Hex(const std::array<std::uint8_t, 32>& hash);
};
