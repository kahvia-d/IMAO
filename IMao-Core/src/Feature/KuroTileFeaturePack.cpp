#include "KuroTileFeaturePack.h"

#include "Processing/FeatureBinaryCodec.h"
#include "Processing/FeatureProcessing.h"
#include "../Coordinate/CoordinateStruct.h"

#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
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
    for (char& character : value) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return value;
}

bool IsSafeRelativeFileName(const std::string& value) {
    const std::filesystem::path relative(value);
    return !value.empty() && !relative.is_absolute() && !relative.has_parent_path() && relative.filename() == relative;
}

bool IsSafeDirectoryName(const std::string& value) {
    const std::filesystem::path relative(value);
    return !value.empty() && !relative.is_absolute() && !relative.has_parent_path() &&
        relative.filename() == relative && value.find('.') == std::string::npos;
}

int HexValue(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    value = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
    return value >= 'a' && value <= 'f' ? value - 'a' + 10 : -1;
}

bool ParseSha256(const std::string& value, std::array<std::uint8_t, 32>& output) {
    if (value.size() != output.size() * 2) return false;
    for (std::size_t index = 0; index < output.size(); ++index) {
        const int high = HexValue(value[index * 2]);
        const int low = HexValue(value[index * 2 + 1]);
        if (high < 0 || low < 0) return false;
        output[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

std::string Sha256File(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("feature file cannot be opened");
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
        for (const auto byte : digest) text << std::setw(2) << static_cast<int>(byte);
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return text.str();
    }
    catch (...) {
        if (hash != nullptr) BCryptDestroyHash(hash);
        if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
        throw;
    }
}

KuroTileFeaturePackStatus Failure(KuroTileFeaturePackStatus status, const std::string& error) {
    status.error = error;
    status.featureData.Release();
    return status;
}
}

std::vector<KuroTileFeaturePackStatus> KuroTileFeaturePack::LoadRegistered(const std::string& featureDataRoot) {
    if (ResourceSnapshotContext::Configured()) {
        std::vector<KuroTileFeaturePackStatus> result;
        for (const auto& package : ResourceSnapshotContext::Snapshot().at("packages")) {
            if (package.at("kind") == "tile")
                result.push_back(LoadDirectory(ResourceSnapshotContext::Path(package.at("directory").get<std::string>())));
        }
        return result;
    }
    const std::filesystem::path featureRoot(featureDataRoot);
    const auto registryPath = featureRoot / "kuro-tile-packs.json";
    if (!std::filesystem::exists(registryPath)) return { LoadPack(featureDataRoot, "Dreamzhou") };
    try {
        std::ifstream input(registryPath);
        if (!input) throw std::runtime_error("tile-pack registry cannot be opened");
        const json registry = json::parse(input);
        if (registry.value("formatVersion", 0) != 1 || !registry.contains("packs") || !registry.at("packs").is_array()) {
            throw std::runtime_error("tile-pack registry format is invalid");
        }
        std::vector<std::string> directories;
        std::vector<KuroTileFeaturePackStatus> result;
        for (const auto& value : registry.at("packs")) {
            const std::string directory = value.get<std::string>();
            if (!IsSafeDirectoryName(directory) || std::find(directories.begin(), directories.end(), directory) != directories.end()) {
                throw std::runtime_error("tile-pack registry contains an invalid or duplicate directory");
            }
            directories.push_back(directory);
            result.push_back(LoadPack(featureDataRoot, directory));
        }
        return result;
    }
    catch (const std::exception& exception) {
        KuroTileFeaturePackStatus status;
        status.directoryName = "registry";
        status.packId = "registry";
        status.error = exception.what();
        return { std::move(status) };
    }
}

KuroTileFeaturePackStatus KuroTileFeaturePack::LoadPack(const std::string& featureDataRoot,
    const std::string& directoryName) {
    KuroTileFeaturePackStatus status;
    status.directoryName = directoryName;
    if (!IsSafeDirectoryName(directoryName)) return Failure(std::move(status), "invalid tile-pack directory");
    const auto packDirectory = std::filesystem::path(featureDataRoot) / "KuroTilePacks" / directoryName;
    return LoadDirectory(packDirectory);
}

KuroTileFeaturePackStatus KuroTileFeaturePack::LoadDirectory(const std::filesystem::path& packDirectory) {
    KuroTileFeaturePackStatus status;
    status.directoryName = packDirectory.filename().string();
    status.directoryPath = packDirectory;
    const auto manifestPath = packDirectory / "manifest.json";
    if (!std::filesystem::exists(manifestPath)) {
        status.error = "not installed";
        return status;
    }
    status.present = true;
    try {
        std::ifstream input(manifestPath);
        if (!input) return Failure(std::move(status), "manifest cannot be opened");
        const json manifest = json::parse(input);
        if (manifest.value("formatVersion", 0) != kSupportedFormatVersion) return Failure(std::move(status), "unsupported manifest format");
        const std::string sceneName = manifest.value("scene", std::string());
        status.sceneId = manifest.value("sceneId", Scene::SceneNameToId(sceneName));
        const auto* scene = Scene::Find(status.sceneId);
        if (scene == nullptr || sceneName != scene->name) return Failure(std::move(status), "manifest scene is invalid");
        status.runtimeApproved = Scene::IsRuntimeApproved(status.sceneId);
        if (manifest.contains("source") && manifest.at("source").contains("state") &&
            manifest.at("source").at("state").get<int>() != scene->kuroStateId) {
            return Failure(std::move(status), "manifest Kuro state does not match scene");
        }
        status.packId = manifest.value("packId", std::string());
        status.resourceVersion = manifest.value("resourceVersion", std::string());
        if (status.packId.empty() || status.resourceVersion.size() != 32) return Failure(std::move(status), "manifest identity is invalid");
        // A tile pack without a field-verified minimap reference can be useful
        // for offline coverage generation, but it must not enter global
        // runtime localization. It otherwise contributes visually plausible
        // yet uncalibrated locations that can defeat a verified World pack.
        const auto& referenceVerification = manifest.value("referenceVerification", json::object());
        if (!referenceVerification.value("passed", false)) {
            return Failure(std::move(status), "field verification is missing or failed");
        }
        // A map-data calibration update must not silently reuse a feature
        // package whose verified anchor was built in a different coordinate
        // system. Signed files can still form an incompatible composition.
        if (ResourceSnapshotContext::Configured()) {
            const double verifiedMinimapScale = manifest.value("minimapScale", 194.0 / 184.0);
            if (!std::isfinite(verifiedMinimapScale) ||
                std::abs(verifiedMinimapScale - Scene::MinimapScale(status.sceneId)) > 0.000001)
                return Failure(std::move(status), "tile package minimap scale is incompatible with selected scene calibration");
            const auto& anchor = manifest.at("anchorWorldCoordinate");
            const auto& expected = referenceVerification.at("expectedMapCoordinate");
            const double anchorX = anchor.at("x").get<double>(), anchorY = anchor.at("y").get<double>();
            const double expectedX = expected.at("x").get<double>(), expectedY = expected.at("y").get<double>();
            if (!std::isfinite(anchorX) || !std::isfinite(anchorY) || !std::isfinite(expectedX) || !std::isfinite(expectedY) ||
                std::hypot(anchorX * scene->scale + scene->originX - expectedX,
                    anchorY * scene->scale + scene->originY - expectedY) > 0.05) {
                return Failure(std::move(status), "tile package verification anchor is incompatible with selected scene calibration");
            }
        }
        const auto& features = manifest.at("features");
        const std::string fileName = features.at("file").get<std::string>();
        const std::string expectedHash = ToLowerAscii(features.at("sha256").get<std::string>());
        const int expectedCount = features.at("keypointCount").get<int>();
        if (!IsSafeRelativeFileName(fileName) || !ParseSha256(expectedHash, status.sourceSha256) || expectedCount < kMinimumPackKeypoints) {
            return Failure(std::move(status), "feature metadata is invalid");
        }
        const auto featurePath = packDirectory / fileName;
        if (!std::filesystem::exists(featurePath)) return Failure(std::move(status), "feature XML is missing");
        const auto binaryPath = packDirectory / "features.imf";
        if (std::filesystem::exists(binaryPath)) {
            FeatureBinaryHeader binaryHeader;
            ImageFeatureData binaryFeatures;
            std::string binaryError;
            if (FeatureBinaryCodec::Load(binaryPath, binaryFeatures, binaryError, &binaryHeader) &&
                binaryHeader.sourceXmlSha256 == status.sourceSha256 &&
                binaryFeatures.imgKeypoints.size() == static_cast<std::size_t>(expectedCount)) {
                status.featureData = std::move(binaryFeatures);
                status.loadedFromBinary = true;
            }
        }
        if (!status.loadedFromBinary) {
            if (Sha256File(featurePath) != expectedHash) return Failure(std::move(status), "feature XML SHA-256 mismatch");
            if (!FeatureLoader::loadFeaturesFromXML(featurePath.string(), status.featureData)) return Failure(std::move(status), "feature XML cannot be read");
        }
        if (status.featureData.imgKeypoints.size() != static_cast<std::size_t>(expectedCount) || status.featureData.imgDescriptors.empty() ||
            status.featureData.imgDescriptors.rows != static_cast<int>(status.featureData.imgKeypoints.size())) {
            return Failure(std::move(status), "feature keypoint and descriptor counts do not agree");
        }
        for (const auto& keypoint : status.featureData.imgKeypoints) {
            if (!std::isfinite(keypoint.pt.x) || !std::isfinite(keypoint.pt.y)) return Failure(std::move(status), "feature XML contains non-finite coordinates");
        }
        status.keypointCount = static_cast<int>(status.featureData.imgKeypoints.size());
        status.loaded = true;
        return status;
    }
    catch (const std::exception& exception) {
        return Failure(std::move(status), exception.what());
    }
}
