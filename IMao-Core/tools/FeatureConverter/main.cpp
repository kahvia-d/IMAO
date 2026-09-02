#include "../../src/Feature/Processing/FeatureBinaryCodec.h"
#include "../../src/Feature/Processing/FeatureProcessing.h"

#include <Windows.h>
#include <Psapi.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

namespace {
bool FeaturesEqual(const ImageFeatureData& expected, const ImageFeatureData& actual, std::string& error) {
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
    const auto byteCount = expected.imgDescriptors.total() * expected.imgDescriptors.elemSize();
    if (std::memcmp(expected.imgDescriptors.data, actual.imgDescriptors.data, byteCount) != 0) {
        error = "round-trip descriptor bytes differ";
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
    if (argumentCount != 4) {
        std::wcerr << L"Usage: IMaoFeatureConverter <Map_features.yml> <Map_features.imf> <manifest.json>\n"
                      L"   or: IMaoFeatureConverter --benchmark <Map_features.imf>\n";
        return 2;
    }
    const std::filesystem::path xmlPath(arguments[1]);
    const std::filesystem::path outputPath(arguments[2]);
    const std::filesystem::path manifestPath(arguments[3]);
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
    if (!FeatureBinaryCodec::Save(temporaryOutput, xmlFeatures, sourceHash, error, &header)) {
        std::cerr << error << '\n';
        return 1;
    }

    ImageFeatureData roundTrip;
    FeatureBinaryHeader loadedHeader;
    if (!FeatureBinaryCodec::Load(temporaryOutput, roundTrip, error, &loadedHeader) ||
        !FeaturesEqual(xmlFeatures, roundTrip, error)) {
        std::filesystem::remove(temporaryOutput);
        std::cerr << error << '\n';
        return 1;
    }

    nlohmann::json manifest = {
        {"format", "IMAOFT01"},
        {"version", header.version},
        {"keypointCount", header.keypointCount},
        {"descriptorRows", header.descriptorRows},
        {"descriptorColumns", header.descriptorColumns},
        {"sourceXmlSha256", FeatureBinaryCodec::Sha256Hex(header.sourceXmlSha256)},
        {"payloadSha256", FeatureBinaryCodec::Sha256Hex(header.payloadSha256)}
    };
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
              << " keypoints and verified an exact round trip.\n";
    return 0;
}
