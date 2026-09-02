#include "CandidateFeaturePack.h"

#include "../Coordinate/locationCalculator/MapCoordinate.h"

#include <bcrypt.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>
#include <sstream>
#include <vector>

using json = nlohmann::json;

namespace {
constexpr int kCurrentFormatVersion = 2;
constexpr int kMinimumCandidateKeypoints = 12;
constexpr float kSelfMatchTolerancePixels = 8.0f;

std::string ToLowerAscii(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

std::string Sha256File(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("reference image cannot be opened");
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<unsigned char> hashObject;
    std::vector<unsigned char> digest;

    try {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
            throw std::runtime_error("SHA-256 provider unavailable");
        }

        DWORD objectLength = 0;
        DWORD hashLength = 0;
        DWORD received = 0;
        if (!BCRYPT_SUCCESS(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &received, 0)) ||
            !BCRYPT_SUCCESS(BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &received, 0))) {
            throw std::runtime_error("SHA-256 property lookup failed");
        }

        hashObject.resize(objectLength);
        digest.resize(hashLength);
        if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, hashObject.data(), objectLength, nullptr, 0, 0))) {
            throw std::runtime_error("SHA-256 initialization failed");
        }

        std::vector<char> buffer(64 * 1024);
        while (input.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || input.gcount() > 0) {
            if (!BCRYPT_SUCCESS(BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()),
                static_cast<ULONG>(input.gcount()), 0))) {
                throw std::runtime_error("SHA-256 read failed");
            }
        }
        if (!input.eof()) {
            throw std::runtime_error("reference image read failed");
        }
        if (!BCRYPT_SUCCESS(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0))) {
            throw std::runtime_error("SHA-256 finalization failed");
        }

        std::ostringstream text;
        text << std::hex << std::setfill('0');
        for (const unsigned char byte : digest) {
            text << std::setw(2) << static_cast<int>(byte);
        }

        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return text.str();
    }
    catch (...) {
        if (hash != nullptr) {
            BCryptDestroyHash(hash);
        }
        if (algorithm != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
        throw;
    }
}

bool IsSafeRelativeFileName(const std::string& value) {
    const std::filesystem::path relative(value);
    if (value.empty() || relative.is_absolute() || relative.has_parent_path()) {
        return false;
    }
    return relative.filename() == relative;
}

bool IsSafeDirectoryName(const std::string& value) {
    const std::filesystem::path relative(value);
    return !value.empty() && !relative.is_absolute() && !relative.has_parent_path() &&
        relative.filename() == relative && value.find('.') == std::string::npos;
}

cv::Mat MakeStableTerrainMask(const cv::Size& imageSize, int innerRadius, int outerRadius) {
    cv::Mat mask = cv::Mat::zeros(imageSize, CV_8UC1);
    const cv::Point center(imageSize.width / 2, imageSize.height / 2);
    cv::circle(mask, center, outerRadius, cv::Scalar(255), cv::FILLED);
    cv::circle(mask, center, innerRadius, cv::Scalar(0), cv::FILLED);
    return mask;
}

ImageFeatureData ExtractMaskedRuntimeSurf(const cv::Mat& image, const cv::Mat& mask) {
    cv::Mat gray;
    cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    auto surf = cv::xfeatures2d::SURF::create(10, 8, 4, true, true);
    std::vector<cv::KeyPoint> keypoints;
    cv::Mat descriptors;
    surf->detectAndCompute(gray, mask, keypoints, descriptors);
    return ImageFeatureData(keypoints, descriptors);
}

ImageFeatureData ExtractUnmaskedRuntimeSurf(const cv::Mat& image) {
    auto surf = cv::xfeatures2d::SURF::create(10, 8, 4, true, true);
    return FeatureMatch::ExtractSurfFeatures(surf, image);
}

void MoveKeypointsToMap(ImageFeatureData& featureData, const Coordinate& anchorMapCoordinate, float scale, const cv::Size& imageSize) {
    const float centerX = imageSize.width / 2.0f;
    const float centerY = imageSize.height / 2.0f;
    for (auto& keypoint : featureData.imgKeypoints) {
        keypoint.pt.x = static_cast<float>(anchorMapCoordinate.x) + scale * (keypoint.pt.x - centerX);
        keypoint.pt.y = static_cast<float>(anchorMapCoordinate.y) + scale * (keypoint.pt.y - centerY);
        keypoint.size *= scale;
    }
}

CandidateFeaturePackStatus Failure(CandidateFeaturePackStatus status, const std::string& error) {
    status.error = error;
    status.featureData.Release();
    return status;
}
}

std::vector<CandidateFeaturePackStatus> CandidateFeaturePack::LoadRegisteredCandidates(
    const std::string& featureDataRoot) {
    const std::filesystem::path featureRoot(featureDataRoot);
    const std::filesystem::path registryPath = featureRoot / "candidate-packs.json";
    std::vector<CandidateFeaturePackStatus> result;
    if (!std::filesystem::exists(registryPath)) {
        // Keep older staged builds usable while their assets are updated.
        result.push_back(LoadCandidate(featureDataRoot, "DreamzhouCandidate"));
        return result;
    }

    try {
        std::ifstream input(registryPath);
        if (!input) throw std::runtime_error("candidate registry cannot be opened");
        const json registry = json::parse(input);
        if (registry.value("formatVersion", 0) != 1 || !registry.contains("packs") ||
            !registry.at("packs").is_array()) {
            throw std::runtime_error("candidate registry format is invalid");
        }

        std::vector<std::string> packDirectories;
        for (const auto& value : registry.at("packs")) {
            const std::string directoryName = value.get<std::string>();
            if (!IsSafeDirectoryName(directoryName) ||
                std::find(packDirectories.begin(), packDirectories.end(), directoryName) != packDirectories.end()) {
                throw std::runtime_error("candidate registry contains an invalid or duplicate pack directory");
            }
            packDirectories.push_back(directoryName);
        }
        for (const auto& directoryName : packDirectories) {
            result.push_back(LoadCandidate(featureDataRoot, directoryName));
        }
    }
    catch (const std::exception& exception) {
        CandidateFeaturePackStatus failure;
        failure.directoryName = "registry";
        failure.packId = "registry";
        failure.error = exception.what();
        result.push_back(std::move(failure));
    }
    return result;
}

CandidateFeaturePackStatus CandidateFeaturePack::LoadCandidate(const std::string& featureDataRoot,
    const std::string& directoryName) {
    CandidateFeaturePackStatus status;
    status.directoryName = directoryName;
    if (!IsSafeDirectoryName(directoryName)) {
        status.error = "invalid candidate pack directory";
        return status;
    }
    const std::filesystem::path packDirectory = std::filesystem::path(featureDataRoot) / directoryName;
    const std::filesystem::path manifestPath = packDirectory / "manifest.json";
    if (!std::filesystem::exists(manifestPath)) {
        status.error = "not installed";
        return status;
    }

    status.present = true;
    try {
        std::ifstream manifestFile(manifestPath);
        if (!manifestFile) {
            return Failure(std::move(status), "manifest cannot be opened");
        }

        const json manifest = json::parse(manifestFile);
        const int formatVersion = manifest.value("formatVersion", 0);
        if (formatVersion < 1 || formatVersion > kCurrentFormatVersion) {
            return Failure(std::move(status), "unsupported manifest format");
        }
        const std::string sceneName = manifest.value("scene", std::string());
        status.sceneId = manifest.value("sceneId", Scene::SceneNameToId(sceneName));
        const auto* scene = Scene::Find(status.sceneId);
        if (scene == nullptr || sceneName != scene->name) {
            return Failure(std::move(status), "candidate scene is invalid");
        }

        status.packId = manifest.value("packId", std::string());
        if (status.packId.empty()) {
            return Failure(std::move(status), "manifest packId is empty");
        }

        json references = json::array();
        if (formatVersion == 1) {
            references.push_back({
                { "anchorWorldCoordinate", manifest.at("anchorWorldCoordinate") },
                { "reference", manifest.at("reference") },
                { "mask", manifest.at("mask") }
            });
        }
        else {
            if (!manifest.contains("references") || !manifest.at("references").is_array() ||
                manifest.at("references").empty()) {
                return Failure(std::move(status), "candidate reference list is missing or empty");
            }
            references = manifest.at("references");
        }

        status.selfMatchAccepted = true;
        for (const auto& entry : references) {
            Coordinate anchorWorldCoordinate;
            const auto& anchor = entry.at("anchorWorldCoordinate");
            anchorWorldCoordinate.x = anchor.at("x").get<double>();
            anchorWorldCoordinate.y = anchor.at("y").get<double>();
            if (!std::isfinite(anchorWorldCoordinate.x) || !std::isfinite(anchorWorldCoordinate.y)) {
                return Failure(std::move(status), "anchor coordinates are invalid");
            }
            const Coordinate anchorMapCoordinate = MapCoordinate::IdentifyCoorToImgMapCoord(anchorWorldCoordinate, status.sceneId);

            const auto& reference = entry.at("reference");
            const std::string imageFileName = reference.at("image").get<std::string>();
            const std::string expectedHash = ToLowerAscii(reference.at("sha256").get<std::string>());
            const int expectedWidth = reference.at("width").get<int>();
            const int expectedHeight = reference.at("height").get<int>();
            if (!IsSafeRelativeFileName(imageFileName) || expectedHash.size() != 64 ||
                expectedWidth <= 0 || expectedHeight <= 0) {
                return Failure(std::move(status), "reference metadata is invalid");
            }

            const auto& maskSettings = entry.contains("mask") ? entry.at("mask") : manifest.at("mask");
            const int innerRadius = maskSettings.at("innerRadius").get<int>();
            const int outerRadius = maskSettings.at("outerRadius").get<int>();
            if (innerRadius <= 0 || outerRadius <= innerRadius ||
                outerRadius > std::min(expectedWidth, expectedHeight) / 2) {
                return Failure(std::move(status), "mask settings are invalid");
            }

            const std::filesystem::path imagePath = packDirectory / imageFileName;
            if (!std::filesystem::exists(imagePath)) {
                return Failure(std::move(status), "reference image is missing: " + imageFileName);
            }
            if (Sha256File(imagePath) != expectedHash) {
                return Failure(std::move(status), "reference image SHA-256 mismatch: " + imageFileName);
            }

            const cv::Mat referenceImage = cv::imread(imagePath.string(), cv::IMREAD_COLOR);
            if (referenceImage.empty() || referenceImage.cols != expectedWidth ||
                referenceImage.rows != expectedHeight) {
                return Failure(std::move(status), "reference image dimensions or decoding failed: " + imageFileName);
            }

            const cv::Mat mask = MakeStableTerrainMask(referenceImage.size(), innerRadius, outerRadius);
            ImageFeatureData referenceFeatures = ExtractMaskedRuntimeSurf(referenceImage, mask);
            if (referenceFeatures.imgDescriptors.empty() ||
                referenceFeatures.imgKeypoints.size() < kMinimumCandidateKeypoints ||
                referenceFeatures.imgDescriptors.rows != static_cast<int>(referenceFeatures.imgKeypoints.size())) {
                return Failure(std::move(status), "reference image has insufficient stable SURF features: " + imageFileName);
            }

            const float mapPixelsPerReferencePixel =
                static_cast<float>(GameWindowsScreenData::minMapOnMap_width) /
                static_cast<float>(referenceImage.cols);
            MoveKeypointsToMap(referenceFeatures, anchorMapCoordinate,
                mapPixelsPerReferencePixel, referenceImage.size());

            const ImageFeatureData unmaskedReference = ExtractUnmaskedRuntimeSurf(referenceImage);
            if (unmaskedReference.imgDescriptors.empty()) {
                return Failure(std::move(status), "reference image self-match descriptors are empty: " + imageFileName);
            }
            const auto selfMatches = FeatureMatch::FindGoodMatchesBetweenMapAndMinMap(
                unmaskedReference, referenceFeatures);
            Coordinate recoveredCoordinate;
            const bool accepted = MapCoordinate::GetGoodPlayerImgMapCoordinateFromMatches(
                referenceImage, selfMatches, referenceFeatures.imgKeypoints,
                unmaskedReference.imgKeypoints, kSelfMatchTolerancePixels,
                anchorMapCoordinate, recoveredCoordinate);
            const double errorPixels = accepted
                ? std::hypot(recoveredCoordinate.x - anchorMapCoordinate.x,
                    recoveredCoordinate.y - anchorMapCoordinate.y)
                : std::numeric_limits<double>::infinity();
            if (!accepted || errorPixels > kSelfMatchTolerancePixels) {
                return Failure(std::move(status), "reference image self-match was rejected: " + imageFileName);
            }

            if (status.referenceCount == 0) {
                status.anchorWorldCoordinate = anchorWorldCoordinate;
                status.anchorMapCoordinate = anchorMapCoordinate;
            }
            if (!status.referenceImage.empty()) status.referenceImage += ';';
            status.referenceImage += imagePath.string();
            ++status.referenceCount;
            status.selfMatchCount += static_cast<int>(selfMatches.size());
            status.selfMatchErrorPixels = std::max(status.selfMatchErrorPixels, errorPixels);
            CandidateFeaturePack::AppendFeatures(status.featureData, referenceFeatures);
        }

        status.keypointCount = static_cast<int>(status.featureData.imgKeypoints.size());
        status.loaded = true;
        return status;
    }
    catch (const std::exception& exception) {
        return Failure(std::move(status), exception.what());
    }
}

void CandidateFeaturePack::AppendFeatures(ImageFeatureData& destination, const ImageFeatureData& addition) {
    if (addition.imgKeypoints.empty() || addition.imgDescriptors.empty()) {
        return;
    }

    destination.imgKeypoints.insert(destination.imgKeypoints.end(), addition.imgKeypoints.begin(), addition.imgKeypoints.end());
    if (destination.imgDescriptors.empty()) {
        destination.imgDescriptors = addition.imgDescriptors.clone();
        return;
    }
    cv::vconcat(destination.imgDescriptors, addition.imgDescriptors, destination.imgDescriptors);
}
