#include "KuroTileFeaturePack.h"

#include "Processing/FeatureProcessing.h"

#include <Windows.h>
#include <bcrypt.h>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <vector>

using json = nlohmann::json;

namespace {
constexpr int kSupportedFormatVersion = 1;
constexpr int kMinimumPackKeypoints = 12;

std::string ToLowerAscii(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

bool IsSafeRelativeFileName(const std::string& value) {
    const std::filesystem::path relative(value);
    return !value.empty() && !relative.is_absolute() && !relative.has_parent_path() && relative.filename() == relative;
}

std::string Sha256File(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) { throw std::runtime_error("feature file cannot be opened"); }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    try {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
            throw std::runtime_error("SHA-256 provider unavailable");
        }
        DWORD objectLength = 0, hashLength = 0, received = 0;
        if (!BCRYPT_SUCCESS(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &received, 0)) ||
            !BCRYPT_SUCCESS(BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &received, 0))) {
            throw std::runtime_error("SHA-256 property lookup failed");
        }
        std::vector<unsigned char> hashObject(objectLength), digest(hashLength);
        if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, hashObject.data(), objectLength, nullptr, 0, 0))) {
            throw std::runtime_error("SHA-256 initialization failed");
        }
        std::vector<char> buffer(64 * 1024);
        while (input.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || input.gcount() > 0) {
            if (!BCRYPT_SUCCESS(BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(input.gcount()), 0))) {
                throw std::runtime_error("SHA-256 read failed");
            }
        }
        if (!input.eof() || !BCRYPT_SUCCESS(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0))) {
            throw std::runtime_error("SHA-256 finalization failed");
        }
        std::ostringstream text;
        text << std::hex << std::setfill('0');
        for (const auto byte : digest) { text << std::setw(2) << static_cast<int>(byte); }
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return text.str();
    }
    catch (...) {
        if (hash != nullptr) { BCryptDestroyHash(hash); }
        if (algorithm != nullptr) { BCryptCloseAlgorithmProvider(algorithm, 0); }
        throw;
    }
}

KuroTileFeaturePackStatus Failure(KuroTileFeaturePackStatus status, const std::string& error) {
    status.error = error;
    status.featureData.Release();
    return status;
}
}

KuroTileFeaturePackStatus KuroTileFeaturePack::LoadDreamzhou(const std::string& featureDataRoot) {
    KuroTileFeaturePackStatus status;
    const std::filesystem::path packDirectory = std::filesystem::path(featureDataRoot) / "KuroTilePacks" / "Dreamzhou";
    const std::filesystem::path manifestPath = packDirectory / "manifest.json";
    if (!std::filesystem::exists(manifestPath)) {
        status.error = "not installed";
        return status;
    }
    status.present = true;
    try {
        std::ifstream input(manifestPath);
        if (!input) { return Failure(std::move(status), "manifest cannot be opened"); }
        const json manifest = json::parse(input);
        if (manifest.value("formatVersion", 0) != kSupportedFormatVersion || manifest.value("scene", std::string()) != "World") {
            return Failure(std::move(status), "unsupported manifest format or scene");
        }
        status.packId = manifest.value("packId", std::string());
        status.resourceVersion = manifest.value("resourceVersion", std::string());
        if (status.packId.empty() || status.resourceVersion.size() != 32) {
            return Failure(std::move(status), "manifest identity is invalid");
        }
        const auto& features = manifest.at("features");
        const std::string fileName = features.at("file").get<std::string>();
        const std::string expectedHash = ToLowerAscii(features.at("sha256").get<std::string>());
        const int expectedCount = features.at("keypointCount").get<int>();
        if (!IsSafeRelativeFileName(fileName) || expectedHash.size() != 64 || expectedCount < kMinimumPackKeypoints) {
            return Failure(std::move(status), "feature metadata is invalid");
        }
        const std::filesystem::path featurePath = packDirectory / fileName;
        if (!std::filesystem::exists(featurePath)) { return Failure(std::move(status), "feature XML is missing"); }
        if (Sha256File(featurePath) != expectedHash) { return Failure(std::move(status), "feature XML SHA-256 mismatch"); }
        if (!FeatureLoader::loadFeaturesFromXML(featurePath.string(), status.featureData)) {
            return Failure(std::move(status), "feature XML cannot be read");
        }
        if (status.featureData.imgKeypoints.size() != static_cast<size_t>(expectedCount) ||
            status.featureData.imgDescriptors.empty() ||
            status.featureData.imgDescriptors.rows != static_cast<int>(status.featureData.imgKeypoints.size())) {
            return Failure(std::move(status), "feature XML keypoint and descriptor counts do not agree");
        }
        for (const auto& keypoint : status.featureData.imgKeypoints) {
            if (!std::isfinite(keypoint.pt.x) || !std::isfinite(keypoint.pt.y)) {
                return Failure(std::move(status), "feature XML contains non-finite coordinates");
            }
        }
        status.keypointCount = static_cast<int>(status.featureData.imgKeypoints.size());
        status.loaded = true;
        return status;
    }
    catch (const std::exception& exception) {
        return Failure(std::move(status), exception.what());
    }
}
