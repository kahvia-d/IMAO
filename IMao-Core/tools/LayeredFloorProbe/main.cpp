// Offline probe for the layered-floor classifier: which floor of a layered map does a
// captured minimap belong to?
//
// It crops the minimap out of a full-screen capture with exactly the geometry
// KuroMapFeatureBuilder uses for reference verification, extracts SURF with the same
// parameters the runtime feeds GlobalVisualLocalizer (60, 8, 4, upright, extended), and
// prints the per-floor vote table so the thresholds can be calibrated against real frames
// before anything depends on them at runtime.
//
//   IMaoLayeredFloorProbe --pack <region pack dir> --reference <shot.png> --full-snapshot

#include "Feature/LayeredFloorIndex.h"

#include <opencv2/opencv.hpp>
#include <opencv2/xfeatures2d.hpp>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

void PrintUsage() {
    std::cerr <<
        "Usage: IMaoLayeredFloorProbe --pack <pack dir> --reference <png>\n"
        "       [--full-snapshot] [--crop x,y,w,h] [--map x,y] [--hessian N] [--min-matches N]\n"
        "       [--margin X] [--ratio X] [--max-distance X] [--no-mask]\n";
}

// The same crop the pack builder applies to a full-screen capture for reference
// verification: the UI is laid out for 1600x900, so the box scales with the frame.
cv::Mat CropReference(const cv::Mat& frame, bool fullSnapshot, cv::Rect explicitCrop) {
    cv::Mat reference = frame;
    if (fullSnapshot) {
        const double horizontal = static_cast<double>(frame.cols) / 1600.0;
        const double vertical = static_cast<double>(frame.rows) / 900.0;
        const int left = cvRound(30.0 * horizontal);
        const int top = cvRound(23.0 * vertical);
        const int right = cvRound(184.0 * horizontal);
        const int bottom = cvRound(177.0 * vertical);
        cv::Rect roi(left, top, right - left, bottom - top);
        roi &= cv::Rect(0, 0, frame.cols, frame.rows);
        if (roi.width <= 0 || roi.height <= 0) throw std::runtime_error("minimap crop is empty");
        cv::resize(frame(roi), reference, cv::Size(184, 184), 0.0, 0.0, cv::INTER_AREA);
    }
    else if (explicitCrop.width > 0 && explicitCrop.height > 0) {
        cv::Rect roi = explicitCrop & cv::Rect(0, 0, frame.cols, frame.rows);
        reference = frame(roi).clone();
    }
    return reference;
}

cv::Mat BuildMinimapMask(const cv::Mat& reference) {
    cv::Mat mask = cv::Mat::zeros(reference.size(), CV_8UC1);
    const int outerRadius = std::min(reference.cols, reference.rows) / 2 - 12;
    const cv::Point centre(reference.cols / 2, reference.rows / 2);
    cv::circle(mask, centre, outerRadius, cv::Scalar(255), cv::FILLED);
    // The player arrow sits in the middle and belongs to no terrain.
    cv::circle(mask, centre, 22, cv::Scalar(0), cv::FILLED);
    return mask;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::filesystem::path> packDirectories;
    std::filesystem::path referencePath;
    bool fullSnapshot = false;
    bool useMask = true;
    cv::Rect crop;
    double hessian = 60.0;
    // Optional player map coordinate, so the footprint check can be exercised alongside the vote.
    bool hasAnchor = false;
    double anchorX = 0.0;
    double anchorY = 0.0;
    // Optional similarity-fit check over the same descriptor matches the localizer uses.
    bool affineMode = false;
    std::string affineFloorId;
    // Same defaults the runtime classifier ships with; see LayeredFloorIndex.h for the
    // calibration these came from.
    int minimumMatches = 10;
    // Descriptors kept per floor at load time; 0 keeps all. Used to check how far the per-floor
    // fingerprint can be cut before floor identification degrades.
    int maxKeypoints = 0;
    // Vote against the capped sample set (what the cold start does) instead of the full one.
    bool useSamples = false;
    double margin = 2.0;
    float ratio = 0.75f;
    float maxDistance = 0.6f;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto next = [&](const char* name) {
            if (index + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + name);
            return std::string(argv[++index]);
        };
        try {
            if (argument == "--pack") packDirectories.push_back(next("--pack"));
            else if (argument == "--reference") referencePath = next("--reference");
            else if (argument == "--full-snapshot") fullSnapshot = true;
            else if (argument == "--no-mask") useMask = false;
            else if (argument == "--hessian") hessian = std::stod(next("--hessian"));
            else if (argument == "--min-matches") minimumMatches = std::stoi(next("--min-matches"));
            else if (argument == "--max-keypoints") maxKeypoints = std::stoi(next("--max-keypoints"));
            else if (argument == "--samples") useSamples = true;
            else if (argument == "--margin") margin = std::stod(next("--margin"));
            else if (argument == "--ratio") ratio = std::stof(next("--ratio"));
            else if (argument == "--max-distance") maxDistance = std::stof(next("--max-distance"));
            else if (argument == "--crop") {
                std::string value = next("--crop");
                std::replace(value.begin(), value.end(), ',', ' ');
                std::istringstream stream(value);
                int x = 0, y = 0, w = 0, h = 0;
                stream >> x >> y >> w >> h;
                crop = cv::Rect(x, y, w, h);
            }
            else if (argument == "--map") {
                std::string value = next("--map");
                std::replace(value.begin(), value.end(), ',', ' ');
                std::istringstream stream(value);
                stream >> anchorX >> anchorY;
                hasAnchor = true;
            }
            else if (argument == "--affine") { affineMode = true; }
            else if (argument == "--affine-floor") { affineMode = true; affineFloorId = next("--affine-floor"); }
            else { PrintUsage(); return 2; }
        }
        catch (const std::exception& exception) {
            std::cerr << exception.what() << '\n';
            return 2;
        }
    }
    if (packDirectories.empty() || referencePath.empty()) { PrintUsage(); return 2; }

    try {
        // Every pack named on the command line contributes its floors, which is how the runtime
        // sees the game: one multiple-choice question over every layered floor at once. Several
        // regions have no layered maps at all, so a missing index there is normal.
        std::vector<LayeredFloors::FloorEntry> floors;
        std::vector<LayeredFloors::Transform> transforms;
        std::vector<std::string> regions;
        std::size_t loadedPacks = 0;
        for (const auto& pack : packDirectories) {
            LayeredFloors::Index index;
            std::string error;
            if (!LayeredFloors::Load(pack, index, error, maxKeypoints)) {
                std::cerr << "  skipped " << pack << ": " << error << '\n';
                continue;
            }
            ++loadedPacks;
            for (auto& floor : index.floors) {
                floors.push_back(std::move(floor));
                transforms.push_back(index.transform);
                regions.push_back(pack.filename().string());
            }
        }
        if (loadedPacks == 0) { std::cerr << "no pack had a floor index\n"; return 1; }
        std::cout << "floor index: " << floors.size() << " floors from " << loadedPacks << " pack(s)\n";
        // Which way each layered map's levels run, so a wrong above/below marker is visible here
        // rather than only in the game.
        {
            std::vector<int> reported;
            for (const auto& floor : floors) {
                if (std::find(reported.begin(), reported.end(), floor.layerId) != reported.end()) continue;
                reported.push_back(floor.layerId);
                std::cout << "  layer " << floor.layerId << " " << floor.layerName
                    << " heightDirection=" << floor.heightDirection << '\n';
            }
        }

        const cv::Mat frame = cv::imread(referencePath.string(), cv::IMREAD_COLOR);
        if (frame.empty()) throw std::runtime_error("cannot read " + referencePath.string());
        const cv::Mat reference = CropReference(frame, fullSnapshot, crop);
        if (reference.empty()) throw std::runtime_error("minimap crop is empty");

        cv::Mat gray;
        cv::cvtColor(reference, gray, cv::COLOR_BGR2GRAY);
        auto surf = cv::xfeatures2d::SURF::create(hessian, 8, 4, true, true);
        ImageFeatureData query;
        if (useMask) {
            const cv::Mat mask = BuildMinimapMask(reference);
            surf->detectAndCompute(gray, mask, query.imgKeypoints, query.imgDescriptors);
        }
        else {
            surf->detectAndCompute(gray, cv::noArray(), query.imgKeypoints, query.imgDescriptors);
        }
        std::cout << "minimap: " << reference.cols << "x" << reference.rows
            << " keypoints=" << query.imgKeypoints.size()
            << " mask=" << (useMask ? "on" : "off") << '\n';

        const auto classification = LayeredFloors::Classify(query, floors, minimumMatches, margin, ratio, maxDistance, useSamples);
        std::cout << "votes (matches):\n";
        for (const auto& vote : classification.votes) {
            const auto found = std::find_if(floors.begin(), floors.end(), [&](const LayeredFloors::FloorEntry& floor) {
                return floor.floorId == vote.floorId;
            });
            const std::string label = found == floors.end() ? vote.floorId : found->floorName;
            // The region matters once every pack is on the table: two regions can name a floor
            // the same way, and a wrong region is a wrong answer even with a right floor name.
            const std::string region = found == floors.end() ? std::string{}
                : regions[static_cast<std::size_t>(std::distance(floors.begin(), found))];
            std::cout << "  " << (vote.floorId == classification.floorId ? "* " : "  ")
                << (region.empty() ? "" : region + " ") << vote.floorId << "  " << label
                << "  " << vote.matches << '\n';
        }
        std::cout << "decision: " << (classification.identified ? "identified" : "unknown")
            << " floor=" << classification.floorId
            << " winner=" << classification.winnerMatches
            << " runnerUp=" << classification.runnerUpMatches
            << " (min=" << minimumMatches << " margin=" << margin << " ratio=" << ratio
            << " maxDistance=" << maxDistance << ")\n";
        // Geometry check: does a similarity fit succeed on the same descriptor matches the
        // localizer uses? Measured per floor set, because several floors can share one tile
        // coordinate and their appearances are then indistinguishable to a plain matcher.
        if (affineMode) {
            const char* label = affineFloorId.empty() ? "all floors" : affineFloorId.c_str();
            std::vector<cv::Point2f> minimapPoints, mapPoints;
            cv::BFMatcher matcher(cv::NORM_L2);
            int considered = 0;
            for (const auto& floor : floors) {
                if (!affineFloorId.empty() && floor.floorId != affineFloorId) continue;
                ++considered;
                std::vector<std::vector<cv::DMatch>> knn;
                matcher.knnMatch(query.imgDescriptors, floor.features.imgDescriptors, knn, 2);
                for (const auto& pair : knn) {
                    if (pair.size() < 2) continue;
                    if (pair[0].distance >= ratio * pair[1].distance || pair[0].distance >= maxDistance) continue;
                    minimapPoints.push_back(query.imgKeypoints[pair[0].queryIdx].pt);
                    mapPoints.push_back(floor.features.imgKeypoints[pair[0].trainIdx].pt);
                }
            }
            std::cout << "geometry over " << label << " (" << considered << " floor(s)): matches="
                << minimapPoints.size();
            if (minimapPoints.size() >= 4) {
                cv::Mat inlierMask;
                const auto transform = cv::estimateAffinePartial2D(minimapPoints, mapPoints, inlierMask,
                    cv::RANSAC, 3.0, 2000, 0.99, 10);
                if (transform.empty()) {
                    std::cout << "  affine=FAILED (empty transform)";
                }
                else {
                    const double a = transform.at<double>(0, 0), b = transform.at<double>(1, 0);
                    std::cout << "  affine=ok scale=" << std::hypot(a, b)
                        << " inliers=" << cv::countNonZero(inlierMask)
                        << " (expected scale 1.0543)";
                }
            }
            std::cout << '\n';
        }
        if (hasAnchor) {
            // Footprint report: what the state machine uses to keep a floor the imagery alone
            // can no longer re-confirm every frame.
            std::cout << "footprint at map (" << anchorX << "," << anchorY << "):\n";
            for (std::size_t i = 0; i < floors.size(); ++i) {
                const auto& floor = floors[i];
                const bool inside = LayeredFloors::Contains(floor, transforms[i], anchorX, anchorY);
                const bool voted = floor.floorId == classification.floorId;
                if (inside || voted) {
                    std::cout << "  " << (inside ? "INSIDE " : "outside") << " " << floor.floorId
                        << "  " << floor.floorName << (voted ? "   (top vote)" : "") << '\n';
                }
            }
        }
        return classification.identified ? 0 : 1;
    }
    catch (const std::exception& exception) {
        std::cerr << "probe failed: " << exception.what() << '\n';
        return 1;
    }
}



