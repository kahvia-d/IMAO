#include "../../src/Feature/Processing/FeatureBinaryCodec.h"
#include "../../src/Feature/Processing/FeatureProcessing.h"

#include <Windows.h>
#include <Psapi.h>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace {
/// Compares a decoded file against the source features. `descriptorTolerance` is
/// the largest per-value difference the stored format can introduce: zero for the
/// legacy float32 encoding, one quantization step for the quantized one. Keypoints
/// are exact in both, because neither format touches them.
bool FeaturesEqual(const ImageFeatureData& expected, const ImageFeatureData& actual,
    float descriptorTolerance, std::string& error) {
    if (expected.imgKeypoints.size() != actual.imgKeypoints.size() ||
        expected.imgDescriptors.size() != actual.imgDescriptors.size() ||
        expected.imgDescriptors.type() != actual.imgDescriptors.type()) {
        error = "round-trip dimensions differ";
        return false;
    }
    for (std::size_t index = 0; index < expected.imgKeypoints.size(); ++index) {
        const auto& left = expected.imgKeypoints[index];
        const auto& right = actual.imgKeypoints[index];
        if (left.pt.x != right.pt.x || left.pt.y != right.pt.y || left.size != right.size ||
            left.angle != right.angle || left.response != right.response ||
            left.octave != right.octave || left.class_id != right.class_id) {
            error = "round-trip keypoint differs at index " + std::to_string(index);
            return false;
        }
    }
    const auto* expectedValues = expected.imgDescriptors.ptr<float>();
    const auto* actualValues = actual.imgDescriptors.ptr<float>();
    const auto valueCount = static_cast<std::size_t>(expected.imgDescriptors.total());
    if (descriptorTolerance <= 0.0f) {
        if (std::memcmp(expected.imgDescriptors.data, actual.imgDescriptors.data,
                valueCount * sizeof(float)) != 0) {
            error = "round-trip descriptor bytes differ";
            return false;
        }
        return true;
    }
    float worst = 0.0f;
    for (std::size_t index = 0; index < valueCount; ++index) {
        const auto difference = std::fabs(expectedValues[index] - actualValues[index]);
        if (difference > worst) worst = difference;
    }
    if (worst > descriptorTolerance) {
        error = "round-trip descriptor deviation " + std::to_string(worst) +
            " exceeds the quantization step " + std::to_string(descriptorTolerance);
        return false;
    }
    return true;
}

bool AtomicReplace(const std::filesystem::path& temporary, const std::filesystem::path& destination) {
    return MoveFileExW(temporary.c_str(), destination.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}
}

int wmain(int argumentCount, wchar_t** arguments) {
    if (argumentCount == 3 && std::wstring(arguments[1]) == L"--benchmark") {
        PROCESS_MEMORY_COUNTERS_EX before{};
        before.cb = sizeof(before);
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&before), sizeof(before));
        const auto start = std::chrono::steady_clock::now();
        ImageFeatureData features;
        std::string error;
        FeatureBinaryHeader header;
        if (!FeatureBinaryCodec::Load(arguments[2], features, error, &header)) {
            std::cerr << error << '\n';
            return 1;
        }
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        PROCESS_MEMORY_COUNTERS_EX after{};
        after.cb = sizeof(after);
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&after), sizeof(after));
        const std::uint64_t additionalWorkingSet = after.WorkingSetSize > before.WorkingSetSize
            ? after.WorkingSetSize - before.WorkingSetSize : 0;
        nlohmann::json result = {
            {"featureLoadMilliseconds", elapsed},
            {"additionalWorkingSetBytes", additionalWorkingSet},
            {"keypointCount", header.keypointCount},
            {"descriptorRows", header.descriptorRows}
        };
        std::cout << result.dump() << '\n';
        return 0;
    }
    // Arguments: [--format v1|v2] <source.xml> <output.imf> <manifest.json>.
    // v2 (quantized descriptors, deflated payloads) is the format new packs use;
    // v1 exists to reproduce a pack an older tool wrote.
    bool legacyFormat = false;
    std::vector<std::wstring> positional;
    for (int index = 1; index < argumentCount; ++index) {
        const std::wstring argument(arguments[index]);
        if (argument == L"--format" && index + 1 < argumentCount) {
            const std::wstring value(arguments[++index]);
            if (value == L"v1" || value == L"legacy") legacyFormat = true;
            else if (value == L"v2") legacyFormat = false;
            else {
                std::wcerr << L"--format accepts v1 or v2.\n";
                return 2;
            }
            continue;
        }
        positional.push_back(argument);
    }
    if (positional.size() != 3) {
        std::wcerr << L"Usage: IMaoFeatureConverter [--format v1|v2] <Map_features.yml> <Map_features.imf> <manifest.json>\n"
                      L"   or: IMaoFeatureConverter --benchmark <Map_features.imf>\n";
        return 2;
    }
    const std::filesystem::path xmlPath(positional[0]);
    const std::filesystem::path outputPath(positional[1]);
    const std::filesystem::path manifestPath(positional[2]);
    const auto temporaryOutput = outputPath.wstring() + L".tmp";
    const auto temporaryManifest = manifestPath.wstring() + L".tmp";

    ImageFeatureData xmlFeatures;
    if (!FeatureLoader::loadFeaturesFromXML(xmlPath.string(), xmlFeatures)) {
        std::cerr << "Unable to load source XML.\n";
        return 1;
    }

    std::array<std::uint8_t, 32> sourceHash{};
    std::string error;
    if (!FeatureBinaryCodec::Sha256File(xmlPath, sourceHash, error)) {
        std::cerr << error << '\n';
        return 1;
    }

    FeatureBinaryHeader header;
    const bool saved = legacyFormat
        ? FeatureBinaryCodec::SaveLegacyFloat32(temporaryOutput, xmlFeatures, sourceHash, error, &header)
        : FeatureBinaryCodec::Save(temporaryOutput, xmlFeatures, sourceHash, error, &header);
    if (!saved) {
        std::cerr << error << '\n';
        return 1;
    }

    ImageFeatureData roundTrip;
    FeatureBinaryHeader loadedHeader;
    if (!FeatureBinaryCodec::Load(temporaryOutput, roundTrip, error, &loadedHeader)) {
        std::filesystem::remove(temporaryOutput);
        std::cerr << error << '\n';
        return 1;
    }
    // The bound is read from the loaded header, not the one written above: it depends
    // on the quantization range the file actually carries. For a legacy file it is
    // zero, which turns the comparison back into an exact byte match.
    if (!FeaturesEqual(xmlFeatures, roundTrip,
            FeatureBinaryCodec::QuantizationTolerance(loadedHeader), error)) {
        std::filesystem::remove(temporaryOutput);
        std::cerr << error << '\n';
        return 1;
    }

    std::error_code sizeError;
    const auto writtenBytes = std::filesystem::file_size(temporaryOutput, sizeError);
    const auto valueCount = static_cast<std::uint64_t>(xmlFeatures.imgDescriptors.total());
    const auto legacyBytes = valueCount * sizeof(float) +
        xmlFeatures.imgKeypoints.size() * FeatureBinaryHeader::KeypointSerializedSize;

    nlohmann::json manifest = {
        {"format", "IMAOFT01"},
        {"version", header.version},
        {"descriptorType", legacyFormat ? "float32" : "quantized-uint8"},
        {"deflated", !legacyFormat},
        {"keypointCount", header.keypointCount},
        {"descriptorRows", header.descriptorRows},
        {"descriptorColumns", header.descriptorColumns},
        {"fileBytes", sizeError ? 0 : writtenBytes},
        {"uncompressedPayloadBytes", legacyBytes},
        {"sourceXmlSha256", FeatureBinaryCodec::Sha256Hex(header.sourceXmlSha256)},
        {"payloadSha256", FeatureBinaryCodec::Sha256Hex(header.payloadSha256)}
    };
    if (!legacyFormat) {
        manifest["descriptorScale"] = header.descriptorScale;
        manifest["descriptorMinimum"] = header.descriptorMinimum;
        manifest["descriptorMaximum"] = header.descriptorMaximum;
        manifest["descriptorOffset"] = header.descriptorOffset;
        manifest["quantizationStep"] = FeatureBinaryCodec::QuantizationTolerance(header);
    }
    {
        std::ofstream file(temporaryManifest, std::ios::binary | std::ios::trunc);
        file << manifest.dump(2) << '\n';
        if (!file) {
            std::filesystem::remove(temporaryOutput);
            std::filesystem::remove(temporaryManifest);
            std::cerr << "Unable to write temporary manifest.\n";
            return 1;
        }
    }

    if (!AtomicReplace(temporaryOutput, outputPath) || !AtomicReplace(temporaryManifest, manifestPath)) {
        std::filesystem::remove(temporaryOutput);
        std::filesystem::remove(temporaryManifest);
        std::cerr << "Unable to atomically replace generated output.\n";
        return 1;
    }

    std::cout << "Generated " << outputPath.string() << " with " << header.keypointCount
              << " keypoints as IMAOFT01 v" << header.version << " ("
              << (legacyFormat ? "float32, uncompressed" : "quantized uint8, deflated")
              << "), verified round trip.\n";
    return 0;
}
