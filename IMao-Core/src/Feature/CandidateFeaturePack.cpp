#include "CandidateFeaturePack.h"

#include "../Coordinate/locationCalculator/MapCoordinate.h"

#include <bcrypt.h>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>
#include <sstream>
#include <vector>

using json = nlohmann::json;

namespace {
constexpr int kSupportedFormatVersion = 1;
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

CandidateFeaturePackStatus CandidateFeaturePack::LoadDreamzhouCandidate(const std::string& featureDataRoot) {
    CandidateFeaturePackStatus status;
    const std::filesystem::path packDirectory = std::filesystem::path(featureDataRoot) / "DreamzhouCandidate";
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
        if (manifest.value("formatVersion", 0) != kSupportedFormatVersion) {
            return Failure(std::move(status), "unsupported manifest format");
        }
        if (manifest.value("scene", std::string()) != "World") {
            return Failure(std::move(status), "candidate scene must be World");
        }

        status.packId = manifest.value("packId", std::string());
        if (status.packId.empty()) {
            return Failure(std::move(status), "manifest packId is empty");
        }

        const auto& anchor = manifest.at("anchorWorldCoordinate");
        status.anchorWorldCoordinate.x = anchor.at("x").get<double>();
        status.anchorWorldCoordinate.y = anchor.at("y").get<double>();
        if (!std::isfinite(status.anchorWorldCoordinate.x) || !std::isfinite(status.anchorWorldCoordinate.y)) {
            return Failure(std::move(status), "anchor coordinates are invalid");
        }
        status.anchorMapCoordinate = MapCoordinate::PlayerWorldCoordToImgMapCoord(status.anchorWorldCoordinate);

        const auto& reference = manifest.at("reference");
        const std::string imageFileName = reference.at("image").get<std::string>();
        const std::string expectedHash = ToLowerAscii(reference.at("sha256").get<std::string>());
        const int expectedWidth = reference.at("width").get<int>();
        const int expectedHeight = reference.at("height").get<int>();
        if (!IsSafeRelativeFileName(imageFileName) || expectedHash.size() != 64 || expectedWidth <= 0 || expectedHeight <= 0) {
            return Failure(std::move(status), "reference metadata is invalid");
        }

        const auto& maskSettings = manifest.at("mask");
        const int innerRadius = maskSettings.at("innerRadius").get<int>();
        const int outerRadius = maskSettings.at("outerRadius").get<int>();
        if (innerRadius <= 0 || outerRadius <= innerRadius || outerRadius > std::min(expectedWidth, expectedHeight) / 2) {
            return Failure(std::move(status), "mask settings are invalid");
        }

        const std::filesystem::path imagePath = packDirectory / imageFileName;
        if (!std::filesystem::exists(imagePath)) {
            return Failure(std::move(status), "reference image is missing");
        }
        const std::string actualHash = Sha256File(imagePath);
        if (actualHash != expectedHash) {
            return Failure(std::move(status), "reference image SHA-256 mismatch");
        }

        const cv::Mat referenceImage = cv::imread(imagePath.string(), cv::IMREAD_COLOR);
        if (referenceImage.empty()) {
            return Failure(std::move(status), "reference image cannot be decoded");
        }
        if (referenceImage.cols != expectedWidth || referenceImage.rows != expectedHeight) {
            return Failure(std::move(status), "reference image dimensions mismatch");
        }

        const cv::Mat mask = MakeStableTerrainMask(referenceImage.size(), innerRadius, outerRadius);
        status.featureData = ExtractMaskedRuntimeSurf(referenceImage, mask);
        if (status.featureData.imgDescriptors.empty() ||
            status.featureData.imgKeypoints.size() < kMinimumCandidateKeypoints ||
            status.featureData.imgDescriptors.rows != static_cast<int>(status.featureData.imgKeypoints.size())) {
            return Failure(std::move(status), "reference image has insufficient stable SURF features");
        }

        const float mapPixelsPerReferencePixel =
            static_cast<float>(GameWindowsScreenData::minMapOnMap_width) / static_cast<float>(referenceImage.cols);
        MoveKeypointsToMap(status.featureData, status.anchorMapCoordinate, mapPixelsPerReferencePixel, referenceImage.size());

        const ImageFeatureData unmaskedReference = ExtractUnmaskedRuntimeSurf(referenceImage);
        if (unmaskedReference.imgDescriptors.empty()) {
            return Failure(std::move(status), "reference image self-match descriptors are empty");
        }
        const auto selfMatches = FeatureMatch::FindGoodMatchesBetweenMapAndMinMap(unmaskedReference, status.featureData);
        Coordinate recoveredCoordinate;
        status.selfMatchAccepted = MapCoordinate::GetGoodPlayerImgMapCoordinateFromMatches(
            referenceImage, selfMatches, status.featureData.imgKeypoints, unmaskedReference.imgKeypoints,
            kSelfMatchTolerancePixels, status.anchorMapCoordinate, recoveredCoordinate);
        status.selfMatchCount = static_cast<int>(selfMatches.size());
        if (status.selfMatchAccepted) {
            status.selfMatchErrorPixels = std::hypot(recoveredCoordinate.x - status.anchorMapCoordinate.x,
                recoveredCoordinate.y - status.anchorMapCoordinate.y);
        }
        if (!status.selfMatchAccepted || status.selfMatchErrorPixels > kSelfMatchTolerancePixels) {
            return Failure(std::move(status), "reference image self-match was rejected");
        }

        status.referenceImage = imagePath.string();
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
