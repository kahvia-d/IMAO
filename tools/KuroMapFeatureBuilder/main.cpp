#include <Windows.h>
#include <bcrypt.h>
#include <opencv2/opencv.hpp>
#include <opencv2/xfeatures2d.hpp>
#include <nlohmann/json.hpp>
#include "../../IMao-Core/src/Coordinate/CoordinateStruct.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
constexpr int kExpectedTileSize = 1024;
constexpr double kKuroVirtualMapSize = 850.0;
constexpr double kWorldToMapScale = 1.205;
constexpr double kWorldOriginX = 2474.0;
constexpr double kWorldOriginY = 1957.0;

struct CandidateFeature {
    cv::KeyPoint keypoint;
    cv::Mat descriptor;
};

struct TileSpec {
    int x = 0;
    int y = 0;
    std::string file;
    std::string sha256;
};

struct CoordinateTransform {
    double originX = 0.0;
    double originY = 0.0;
    double scale = 0.0;
};

struct BuildSummary {
    std::string packId;
    std::string resourceVersion;
    size_t tileCount = 0;
    size_t extractedKeypoints = 0;
    size_t selectedKeypoints = 0;
    double minX = 0;
    double maxX = 0;
    double minY = 0;
    double maxY = 0;
};

struct ReferenceVerification {
    bool passed = false;
    size_t mapKeypoints = 0;
    size_t minimapKeypoints = 0;
    size_t goodMatches = 0;
    size_t nearAnchorMatches = 0;
    double expectedMapX = 0;
    double expectedMapY = 0;
    double resultMapX = 0;
    double resultMapY = 0;
    double errorPixels = 0;
};

std::string ToLowerAscii(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

bool IsSafeRelativePath(const std::string& value) {
    const fs::path relative(value);
    return !value.empty() && !relative.is_absolute() && !relative.has_root_name() &&
        std::none_of(relative.begin(), relative.end(), [](const fs::path& part) { return part == ".."; });
}

std::string Sha256File(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open file for SHA-256: " + path.string());
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    try {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
            throw std::runtime_error("SHA-256 provider unavailable");
        }
        DWORD objectLength = 0;
        DWORD hashLength = 0;
        DWORD received = 0;
        if (!BCRYPT_SUCCESS(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &received, 0)) ||
            !BCRYPT_SUCCESS(BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &received, 0))) {
            throw std::runtime_error("SHA-256 property lookup failed");
        }
        std::vector<unsigned char> hashObject(objectLength);
        std::vector<unsigned char> digest(hashLength);
        if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, hashObject.data(), objectLength, nullptr, 0, 0))) {
            throw std::runtime_error("SHA-256 initialization failed");
        }
        std::array<char, 64 * 1024> buffer{};
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
        for (const auto byte : digest) {
            text << std::setw(2) << static_cast<int>(byte);
        }
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

cv::Point2f KuroTilePointToAppMap(const int tileX, const int tileY, const cv::Point2f& tilePoint,
    const CoordinateTransform& transform) {
    // Kuro's public front end exposes the inverse of this mapping:
    // game -> [game.x * 1024/850 + 1024, -game.y * 1024/850].
    // The raw tile URL flips Leaflet's Y coordinate, hence tileY * 1024 - pixelY.
    const double kuroX = static_cast<double>(tileX) * kExpectedTileSize + tilePoint.x;
    const double kuroY = static_cast<double>(tileY) * kExpectedTileSize - tilePoint.y;
    const double worldX = (kuroX - kExpectedTileSize) * kKuroVirtualMapSize / kExpectedTileSize;
    const double worldY = -kuroY * kKuroVirtualMapSize / kExpectedTileSize;
    return cv::Point2f(
        static_cast<float>(worldX * transform.scale + transform.originX),
        static_cast<float>(worldY * transform.scale + transform.originY));
}

std::vector<TileSpec> ReadTileManifest(const fs::path& manifestPath, BuildSummary& summary,
    CoordinateTransform& transform) {
    std::ifstream input(manifestPath);
    if (!input) { throw std::runtime_error("cannot open tile manifest"); }
    const json manifest = json::parse(input);
    if (manifest.value("formatVersion", 0) != 1) throw std::runtime_error("unsupported tile manifest format");
    const std::string sceneName = manifest.value("scene", std::string());
    const int sceneId = manifest.value("sceneId", Scene::SceneNameToId(sceneName));
    const auto* scene = Scene::Find(sceneId);
    if (scene == nullptr || sceneName != scene->name) throw std::runtime_error("unsupported tile manifest scene");
    if (manifest.contains("source") && manifest.at("source").contains("state") &&
        manifest.at("source").at("state").get<int>() != scene->kuroStateId) {
        throw std::runtime_error("tile manifest state does not match scene");
    }
    const auto& transformNode = manifest.contains("coordinateTransform") ? manifest.at("coordinateTransform") : json::object();
    transform.originX = transformNode.value("originX", scene->originX);
    transform.originY = transformNode.value("originY", scene->originY);
    transform.scale = transformNode.value("scale", scene->scale);
    if (!std::isfinite(transform.originX) || !std::isfinite(transform.originY) ||
        !std::isfinite(transform.scale) || transform.scale <= 0.0) {
        throw std::runtime_error("tile manifest coordinate transform is invalid");
    }
    summary.packId = manifest.value("packId", std::string());
    summary.resourceVersion = manifest.value("resourceVersion", std::string());
    if (summary.packId.empty() || summary.resourceVersion.empty() || !manifest.contains("tiles") || !manifest.at("tiles").is_array()) {
        throw std::runtime_error("tile manifest is missing packId, resourceVersion, or tiles");
    }
    std::vector<TileSpec> tiles;
    for (const auto& node : manifest.at("tiles")) {
        TileSpec tile;
        tile.x = node.at("x").get<int>();
        tile.y = node.at("y").get<int>();
        tile.file = node.at("file").get<std::string>();
        tile.sha256 = ToLowerAscii(node.at("sha256").get<std::string>());
        if (!IsSafeRelativePath(tile.file) || tile.sha256.size() != 64) {
            throw std::runtime_error("tile manifest contains unsafe file name or invalid SHA-256");
        }
        tiles.push_back(std::move(tile));
    }
    if (tiles.empty()) { throw std::runtime_error("tile manifest contains no tiles"); }
    return tiles;
}

void WriteJson(const fs::path& path, const json& value) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) { throw std::runtime_error("cannot write report: " + path.string()); }
    output << value.dump(2) << '\n';
}

void WriteFeatures(const fs::path& outputPath, const std::vector<CandidateFeature>& selected) {
    std::vector<cv::KeyPoint> keypoints;
    keypoints.reserve(selected.size());
    cv::Mat descriptors;
    for (const auto& feature : selected) {
        keypoints.push_back(feature.keypoint);
        descriptors.push_back(feature.descriptor);
    }
    if (keypoints.empty() || descriptors.rows != static_cast<int>(keypoints.size())) {
        throw std::runtime_error("feature selection produced an invalid descriptor matrix");
    }
    fs::create_directories(outputPath.parent_path());
    cv::FileStorage file(outputPath.string(), cv::FileStorage::WRITE | cv::FileStorage::FORMAT_XML);
    if (!file.isOpened()) { throw std::runtime_error("cannot create feature XML"); }
    file << "num_keypoints" << static_cast<int>(keypoints.size());
    file << "keypoints" << keypoints;
    file << "descriptors" << descriptors;
    file.release();
}

BuildSummary Build(const fs::path& manifestPath, const fs::path& outputPath, CoordinateTransform& transform) {
    BuildSummary summary;
    const auto tiles = ReadTileManifest(manifestPath, summary, transform);
    const fs::path tileRoot = manifestPath.parent_path();
    std::vector<CandidateFeature> selected;
    auto surf = cv::xfeatures2d::SURF::create(60, 8, 4, true, true);

    bool hasBounds = false;
    for (const auto& tile : tiles) {
        const fs::path imagePath = tileRoot / fs::path(tile.file);
        if (!fs::exists(imagePath)) { throw std::runtime_error("tile is missing: " + tile.file); }
        if (Sha256File(imagePath) != tile.sha256) { throw std::runtime_error("tile SHA-256 mismatch: " + tile.file); }
        const cv::Mat image = cv::imread(imagePath.string(), cv::IMREAD_COLOR);
        if (image.empty() || image.cols != kExpectedTileSize || image.rows != kExpectedTileSize) {
            throw std::runtime_error("tile is not a 1024x1024 PNG: " + tile.file);
        }
        cv::Mat gray;
        cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
        std::vector<cv::KeyPoint> keypoints;
        cv::Mat descriptors;
        surf->detectAndCompute(gray, cv::noArray(), keypoints, descriptors);
        if (keypoints.size() != static_cast<size_t>(descriptors.rows)) {
            throw std::runtime_error("SURF descriptors do not align with keypoints: " + tile.file);
        }
        summary.extractedKeypoints += keypoints.size();
        for (int index = 0; index < descriptors.rows; ++index) {
            cv::KeyPoint mapped = keypoints[static_cast<size_t>(index)];
            mapped.pt = KuroTilePointToAppMap(tile.x, tile.y, mapped.pt, transform);
            if (!std::isfinite(mapped.pt.x) || !std::isfinite(mapped.pt.y)) {
                throw std::runtime_error("mapped feature coordinate is invalid");
            }
            if (!hasBounds) {
                summary.minX = summary.maxX = mapped.pt.x;
                summary.minY = summary.maxY = mapped.pt.y;
                hasBounds = true;
            }
            else {
                summary.minX = std::min(summary.minX, static_cast<double>(mapped.pt.x));
                summary.maxX = std::max(summary.maxX, static_cast<double>(mapped.pt.x));
                summary.minY = std::min(summary.minY, static_cast<double>(mapped.pt.y));
                summary.maxY = std::max(summary.maxY, static_cast<double>(mapped.pt.y));
            }
            // A minimap sees only a small terrain annulus. Ranking a 420px
            // cell by response discarded its quiet terrain in favour of dense
            // city/contour clusters elsewhere in that cell. Preserve detected
            // observations; the visual index bounds runtime retrieval instead.
            selected.push_back(CandidateFeature{ mapped, descriptors.row(index).clone() });
        }
        ++summary.tileCount;
        std::cout << "processed tile " << tile.x << ',' << tile.y << " features=" << keypoints.size() << '\n';
    }

    std::sort(selected.begin(), selected.end(), [](const CandidateFeature& first, const CandidateFeature& second) {
        if (first.keypoint.pt.y != second.keypoint.pt.y) { return first.keypoint.pt.y < second.keypoint.pt.y; }
        return first.keypoint.pt.x < second.keypoint.pt.x;
    });
    summary.selectedKeypoints = selected.size();
    if (summary.selectedKeypoints < 12) { throw std::runtime_error("too few selected SURF features"); }
    WriteFeatures(outputPath, selected);
    return summary;
}

double Median(std::vector<double> values) {
    if (values.empty()) { throw std::runtime_error("cannot find median of an empty set"); }
    std::sort(values.begin(), values.end());
    const size_t middle = values.size() / 2;
    return values.size() % 2 == 1 ? values[middle] : (values[middle - 1] + values[middle]) / 2.0;
}

ReferenceVerification VerifyReference(const fs::path& featurePath, const fs::path& referencePath,
    double anchorWorldX, double anchorWorldY, const CoordinateTransform& transform,
    bool referenceIsFullSnapshot) {
    if (!fs::exists(referencePath)) { throw std::runtime_error("reference minimap is missing"); }
    std::vector<cv::KeyPoint> allMapKeypoints;
    cv::Mat allMapDescriptors;
    cv::FileStorage featureFile(featurePath.string(), cv::FileStorage::READ | cv::FileStorage::FORMAT_XML);
    if (!featureFile.isOpened()) { throw std::runtime_error("cannot open generated feature XML for verification"); }
    featureFile["keypoints"] >> allMapKeypoints;
    featureFile["descriptors"] >> allMapDescriptors;
    featureFile.release();
    if (allMapKeypoints.empty() || allMapDescriptors.rows != static_cast<int>(allMapKeypoints.size())) {
        throw std::runtime_error("generated feature XML is invalid during reference verification");
    }
    cv::Mat reference = cv::imread(referencePath.string(), cv::IMREAD_COLOR);
    if (reference.empty()) { throw std::runtime_error("cannot read reference minimap"); }
    if (referenceIsFullSnapshot) {
        // Match ImageProcessing::CropToMinMapAreaImg's 1600x900 UI geometry.
        // This is deliberately an opt-in diagnostic path: production packs
        // continue to store compact minimap references, while field captures
        // can be validated directly without hand-cropping a screenshot.
        const double horizontalFactor = static_cast<double>(reference.cols) / 1600.0;
        const double verticalFactor = static_cast<double>(reference.rows) / 900.0;
        const int left = cvRound(30.0 * horizontalFactor);
        const int top = cvRound(23.0 * verticalFactor);
        const int right = cvRound(184.0 * horizontalFactor);
        const int bottom = cvRound(177.0 * verticalFactor);
        const cv::Rect minimapRoi(left, top, right - left, bottom - top);
        if (minimapRoi.width <= 0 || minimapRoi.height <= 0 || minimapRoi.x < 0 || minimapRoi.y < 0 ||
            minimapRoi.x + minimapRoi.width > reference.cols ||
            minimapRoi.y + minimapRoi.height > reference.rows) {
            throw std::runtime_error("full-snapshot minimap crop is outside the reference image");
        }
        reference = reference(minimapRoi).clone();
        cv::resize(reference, reference, cv::Size(184, 184), 0.0, 0.0, cv::INTER_AREA);
    }
    cv::Mat mask = cv::Mat::zeros(reference.size(), CV_8UC1);
    const int outerRadius = std::min(reference.cols, reference.rows) / 2 - 12;
    cv::circle(mask, cv::Point(reference.cols / 2, reference.rows / 2), outerRadius, cv::Scalar(255), cv::FILLED);
    cv::circle(mask, cv::Point(reference.cols / 2, reference.rows / 2), 22, cv::Scalar(0), cv::FILLED);
    cv::Mat referenceGray;
    cv::cvtColor(reference, referenceGray, cv::COLOR_BGR2GRAY);
    auto surf = cv::xfeatures2d::SURF::create(60, 8, 4, true, true);
    std::vector<cv::KeyPoint> minimapKeypoints;
    cv::Mat minimapDescriptors;
    surf->detectAndCompute(referenceGray, mask, minimapKeypoints, minimapDescriptors);

    ReferenceVerification verification;
    verification.minimapKeypoints = minimapKeypoints.size();
    const double expectedX = anchorWorldX * transform.scale + transform.originX;
    const double expectedY = anchorWorldY * transform.scale + transform.originY;
    verification.expectedMapX = expectedX;
    verification.expectedMapY = expectedY;
    if (minimapDescriptors.empty()) { return verification; }

    std::vector<cv::KeyPoint> nearbyMapKeypoints;
    cv::Mat nearbyMapDescriptors;
    for (int index = 0; index < allMapDescriptors.rows; ++index) {
        const auto& point = allMapKeypoints[static_cast<size_t>(index)].pt;
        if (std::abs(point.x - expectedX) <= 160.0 && std::abs(point.y - expectedY) <= 160.0) {
            nearbyMapKeypoints.push_back(allMapKeypoints[static_cast<size_t>(index)]);
            nearbyMapDescriptors.push_back(allMapDescriptors.row(index));
        }
    }
    verification.mapKeypoints = nearbyMapKeypoints.size();
    if (nearbyMapDescriptors.rows < 2) { return verification; }

    cv::BFMatcher matcher(cv::NORM_L2);
    std::vector<std::vector<cv::DMatch>> matches;
    matcher.knnMatch(nearbyMapDescriptors, minimapDescriptors, matches, 2);
    std::vector<double> estimateXs;
    std::vector<double> estimateYs;
    const double minimapToMapScale = 194.0 / reference.cols;
    for (const auto& pair : matches) {
        if (pair.size() < 2 || pair[0].distance >= 0.65f * pair[1].distance || pair[0].distance >= 0.5f) { continue; }
        ++verification.goodMatches;
        const auto& mapPoint = nearbyMapKeypoints[static_cast<size_t>(pair[0].queryIdx)].pt;
        const auto& minimapPoint = minimapKeypoints[static_cast<size_t>(pair[0].trainIdx)].pt;
        const double estimateX = mapPoint.x - minimapToMapScale * (minimapPoint.x - reference.cols / 2.0);
        const double estimateY = mapPoint.y - minimapToMapScale * (minimapPoint.y - reference.rows / 2.0);
        if (std::abs(estimateX - expectedX) <= 8.0 && std::abs(estimateY - expectedY) <= 8.0) {
            estimateXs.push_back(estimateX);
            estimateYs.push_back(estimateY);
        }
    }
    verification.nearAnchorMatches = estimateXs.size();
    if (estimateXs.empty()) { return verification; }
    verification.resultMapX = Median(std::move(estimateXs));
    verification.resultMapY = Median(std::move(estimateYs));
    verification.errorPixels = std::hypot(verification.resultMapX - expectedX, verification.resultMapY - expectedY);
    verification.passed = verification.errorPixels <= 8.0;
    return verification;
}

void PrintUsage() {
    std::cerr << "Usage: KuroMapFeatureBuilder --input <tiles.json> --output <features.yml> --report <report.json> [--verify-reference <png> --reference-full-snapshot --anchor-x <x> --anchor-y <y>]\n";
}
}

int main(int argc, char** argv) {
    fs::path inputPath;
    fs::path outputPath;
    fs::path reportPath;
    fs::path referencePath;
    bool referenceIsFullSnapshot = false;
    bool hasAnchorX = false;
    bool hasAnchorY = false;
    double anchorWorldX = 0;
    double anchorWorldY = 0;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--input" && index + 1 < argc) { inputPath = argv[++index]; }
        else if (argument == "--output" && index + 1 < argc) { outputPath = argv[++index]; }
        else if (argument == "--report" && index + 1 < argc) { reportPath = argv[++index]; }
        else if (argument == "--verify-reference" && index + 1 < argc) { referencePath = argv[++index]; }
        else if (argument == "--reference-full-snapshot") { referenceIsFullSnapshot = true; }
        else if (argument == "--anchor-x" && index + 1 < argc) { anchorWorldX = std::stod(argv[++index]); hasAnchorX = true; }
        else if (argument == "--anchor-y" && index + 1 < argc) { anchorWorldY = std::stod(argv[++index]); hasAnchorY = true; }
        else { PrintUsage(); return 2; }
    }
    if (inputPath.empty() || outputPath.empty() || reportPath.empty()) { PrintUsage(); return 2; }
    if ((!referencePath.empty() && (!hasAnchorX || !hasAnchorY)) ||
        (referencePath.empty() && (hasAnchorX || hasAnchorY || referenceIsFullSnapshot))) { PrintUsage(); return 2; }
    try {
        CoordinateTransform transform;
        const BuildSummary summary = Build(inputPath, outputPath, transform);
        json report = {
            { "formatVersion", 1 },
            { "packId", summary.packId },
            { "resourceVersion", summary.resourceVersion },
            { "tileCount", summary.tileCount },
            { "extractedKeypoints", summary.extractedKeypoints },
            { "selectedKeypoints", summary.selectedKeypoints },
            { "coordinateBounds", {{ "minX", summary.minX }, { "maxX", summary.maxX }, { "minY", summary.minY }, { "maxY", summary.maxY }} },
            { "featuresSha256", Sha256File(outputPath) }
        };
        if (!referencePath.empty()) {
            const ReferenceVerification verification = VerifyReference(outputPath, referencePath,
                anchorWorldX, anchorWorldY, transform, referenceIsFullSnapshot);
            report["referenceVerification"] = {
                { "reference", referencePath.filename().string() },
                { "passed", verification.passed },
                { "mapKeypoints", verification.mapKeypoints },
                { "minimapKeypoints", verification.minimapKeypoints },
                { "goodMatches", verification.goodMatches },
                { "nearAnchorMatches", verification.nearAnchorMatches },
                { "expectedMapCoordinate", {{ "x", verification.expectedMapX }, { "y", verification.expectedMapY }} },
                { "resultMapCoordinate", {{ "x", verification.resultMapX }, { "y", verification.resultMapY }} },
                { "errorPixels", verification.errorPixels }
            };
            if (!verification.passed) {
                WriteJson(reportPath, report);
                std::cerr << "KuroMapFeatureBuilder failed: reference minimap did not lock within 8 pixels\n";
                return 1;
            }
        }
        WriteJson(reportPath, report);
        std::cout << "complete tiles=" << summary.tileCount << " extracted=" << summary.extractedKeypoints << " selected=" << summary.selectedKeypoints << '\n';
        return 0;
    }
    catch (const std::exception& exception) {
        std::cerr << "KuroMapFeatureBuilder failed: " << exception.what() << '\n';
        return 1;
    }
}
