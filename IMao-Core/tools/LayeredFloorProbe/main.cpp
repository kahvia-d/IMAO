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
        "       [--min-own-matches N] [--margin X] [--ratio X] [--max-distance X] [--no-mask]\n"
        "       [--dump-matches <floorId>]\n"
        "       --batch <list> [--only-floor <ids>]     list = '<label>\\t<image>' per line\n";
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

/// Batch mode: the list holds one `<label>\t<image>` per line, and one CSV line comes back per
/// query. The packs are loaded once for the whole run, which is what makes a scan over every floor
/// of the game practical - the single-query path reloads ~275k descriptors per invocation.
int RunBatch(const std::string& batchPath, const std::vector<LayeredFloors::FloorEntry>& floors,
    const std::vector<std::string>& floorFilter, bool fullSnapshot, cv::Rect crop, bool useMask,
    double hessian, int minimumMatches, int minimumOwnMatches, double margin, float ratio,
    float maxDistance, bool useSamples, bool dumpVotes) {
    std::ifstream list(batchPath);
    if (!list) { std::cerr << "cannot open batch list " << batchPath << '\n'; return 2; }

    std::vector<const LayeredFloors::FloorEntry*> candidates;
    for (const auto& floor : floors) {
        if (floorFilter.empty() ||
            std::find(floorFilter.begin(), floorFilter.end(), floor.floorId) != floorFilter.end()) {
            candidates.push_back(&floor);
        }
    }
    std::cout << "label,identified,floor,winner,runnerUp,ownIdentified,ownFloor,ownWinner,ownRunnerUp,keypoints\n";
    std::string line;
    while (std::getline(list, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const auto tab = line.find('\t');
        const std::string label = tab == std::string::npos ? line : line.substr(0, tab);
        const std::string path = tab == std::string::npos ? std::string() : line.substr(tab + 1);
        if (path.empty()) continue;
        const cv::Mat frame = cv::imread(path, cv::IMREAD_COLOR);
        if (frame.empty()) { std::cout << label << ",ERR,,,,,,,,\n"; continue; }
        const cv::Mat reference = CropReference(frame, fullSnapshot, crop);
        if (reference.empty()) { std::cout << label << ",ERR,,,,,,,,\n"; continue; }
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
        const auto classification = LayeredFloors::Classify(query, candidates, minimumMatches, margin,
            ratio, maxDistance, useSamples, minimumOwnMatches);
        std::cout << label << ',' << (classification.identified ? 1 : 0) << ','
            << classification.floorId << ',' << classification.winnerMatches << ','
            << classification.runnerUpMatches << ',' << (classification.ownIdentified ? 1 : 0) << ','
            << classification.ownFloorId << ',' << classification.winnerOwnMatches << ','
            << classification.runnerUpOwnMatches << ',' << query.imgKeypoints.size();
        if (dumpVotes) {
            // The full table, "<floorId>=<own>:<total>" joined by ';'. A sweep gets every candidate
            // floor's own-art vote for every query, so thresholds and margins can be searched
            // offline instead of by re-running the scanner once per candidate setting.
            std::cout << ',';
            for (std::size_t index = 0; index < classification.ownVotes.size(); ++index) {
                const auto& vote = classification.ownVotes[index];
                if (index != 0) std::cout << ';';
                std::cout << vote.floorId << '=' << vote.ownMatches << ':' << vote.matches;
            }
        }
        std::cout << '\n';
    }
    return 0;
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
    // Print every accepted match against one floor as "queryX queryY mapX mapY distance own", so a
    // plot can show WHICH part of a floor matched and whether the two images are the same place
    // there. Added for the 虎口山脉 investigation - see Docs/LayeredMapFalsePositive_Hukou_20260926.md.
    bool dumpMatches = false;
    std::string dumpFloorId;
    // Whole-game sweep support: a list of queries to run in one process, optionally against a
    // narrowed candidate set. See RunBatch.
    std::string batchPath;
    std::string onlyFloors;
    bool dumpVotes = false;
    // Same defaults the runtime classifier ships with; see LayeredFloorIndex.h for the
    // calibration these came from.
    int minimumMatches = 10;
    // The bar for the own-art vote, which is what decides the floor's identity. Separate flag so a
    // threshold can be searched over real frames without rebuilding - see
    // Docs/LayeredMapFalsePositive_Hukou_20260926.md.
    int minimumOwnMatches = 8;
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
            else if (argument == "--min-own-matches") minimumOwnMatches = std::stoi(next("--min-own-matches"));
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
            else if (argument == "--dump-matches") { dumpMatches = true; dumpFloorId = next("--dump-matches"); }
            else if (argument == "--batch") batchPath = next("--batch");
            else if (argument == "--only-floor") onlyFloors = next("--only-floor");
            else if (argument == "--dump-votes") dumpVotes = true;
            else { PrintUsage(); return 2; }
        }
        catch (const std::exception& exception) {
            std::cerr << exception.what() << '\n';
            return 2;
        }
    }
    if (batchPath.empty() && (packDirectories.empty() || referencePath.empty())) { PrintUsage(); return 2; }
    if (!batchPath.empty() && packDirectories.empty()) { PrintUsage(); return 2; }

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

        // --only-floor narrows the candidate set. A floor can only ever be adopted when its
        // footprint contains the player, and each floor's vote is computed independently of the
        // others, so a sweep over thousands of synthetic queries can classify one floor at a time
        // and still reconstruct the full ranking afterwards - which is what makes a 90-floor scan
        // finish in minutes instead of hours. The single-query path below is left untouched.
        std::vector<std::string> floorFilter;
        if (!onlyFloors.empty()) {
            std::string value = onlyFloors;
            std::replace(value.begin(), value.end(), ',', ' ');
            std::istringstream stream(value);
            std::string id;
            while (stream >> id) floorFilter.push_back(id);
        }

        if (!batchPath.empty()) {
            return RunBatch(batchPath, floors, floorFilter, fullSnapshot, crop, useMask, hessian,
                minimumMatches, minimumOwnMatches, margin, ratio, maxDistance, useSamples, dumpVotes);
        }

        std::cout << "floor index: " << floors.size() << " floors from " << loadedPacks << " pack(s)\n";
        // Where each floor sits, so a wrong above/below marker is visible here rather than only in
        // the game. The rank is what the markers use; the direction is only its fallback.
        {
            std::vector<int> reported;
            for (const auto& floor : floors) {
                if (std::find(reported.begin(), reported.end(), floor.layerId) == reported.end()) {
                    reported.push_back(floor.layerId);
                    std::cout << "  layer " << floor.layerId << " " << floor.layerName
                        << " heightDirection=" << floor.heightDirection << '\n';
                    for (const auto& member : floors) {
                        if (member.layerId != floor.layerId) continue;
                        std::cout << "    " << member.floorId << "  " << member.floorName
                            << "  heightRank=" << member.heightRank
                            << (LayeredFloors::AdjacentToSurface(member) ? "  adjacent=yes" : "") << '\n';
                    }
                }
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

        const auto classification = LayeredFloors::Classify(query, floors, minimumMatches, margin, ratio, maxDistance, useSamples, minimumOwnMatches);
        std::cout << "votes (matches / own-art):\n";
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
                << "  " << vote.matches << " / " << vote.ownMatches << '\n';
        }
        std::cout << "decision: " << (classification.identified ? "identified" : "unknown")
            << " floor=" << classification.floorId
            << " winner=" << classification.winnerMatches
            << " runnerUp=" << classification.runnerUpMatches << "   [total imagery]"
            << " (min=" << minimumMatches << " margin=" << margin << " ratio=" << ratio
            << " maxDistance=" << maxDistance << ")\n";
        std::cout << "identity: " << (classification.ownIdentified ? "identified" : "unknown")
            << " floor=" << (classification.ownFloorId.empty() ? "-" : classification.ownFloorId)
            << " winner=" << classification.winnerOwnMatches
            << " runnerUp=" << classification.runnerUpOwnMatches << "   [layer's own art only]"
            << " (minOwn=" << minimumOwnMatches << ")\n";
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
        if (dumpMatches) {
            // The same knnMatch + ratio test Classify runs, but one line per accepted match: where it
            // landed in the query image, and where that descriptor sits in the map.
            for (const auto& floor : floors) {
                if (floor.floorId != dumpFloorId) continue;
                const bool hasOwn = floor.ownArt.size() ==
                    static_cast<std::size_t>(floor.features.imgDescriptors.rows);
                cv::BFMatcher matcher(cv::NORM_L2);
                std::vector<std::vector<cv::DMatch>> knn;
                matcher.knnMatch(query.imgDescriptors, floor.features.imgDescriptors, knn, 2);
                for (const auto& pair : knn) {
                    if (pair.size() < 2) continue;
                    if (pair[0].distance >= ratio * pair[1].distance || pair[0].distance >= maxDistance) continue;
                    const auto& queryPoint = query.imgKeypoints[pair[0].queryIdx].pt;
                    const auto& mapPoint = floor.features.imgKeypoints[pair[0].trainIdx].pt;
                    const int own = hasOwn && floor.ownArt[pair[0].trainIdx] != 0 ? 1 : 0;
                    std::cout << "match " << queryPoint.x << " " << queryPoint.y << " "
                        << mapPoint.x << " " << mapPoint.y << " " << pair[0].distance << " " << own << '\n';
                }
            }
        }
        if (hasAnchor) {
            // Footprint report: what the state machine uses to keep a floor the imagery alone
            // can no longer re-confirm every frame. The shared columns are the other half of
            // that decision: `shared` is how much of the art around the anchor is the surface's
            // own pixels, `copied` is that share over the whole floor, and `adjacent` says whether
            // the floor is the one next to the surface at all - only there can the comparison
            // mean anything, because every other floor is above or below the surface.
            std::cout << "footprint at map (" << anchorX << "," << anchorY << "):\n";
            for (std::size_t i = 0; i < floors.size(); ++i) {
                const auto& floor = floors[i];
                const bool inside = LayeredFloors::Contains(floor, transforms[i], anchorX, anchorY);
                const bool voted = floor.floorId == classification.floorId;
                if (inside || voted) {
                    const bool adjacent = LayeredFloors::AdjacentToSurface(floor);
                    std::cout << "  " << (inside ? "INSIDE " : "outside") << " " << floor.floorId
                        << "  " << floor.floorName << (voted ? "   (top vote)" : "")
                        << "  copied=" << floor.copiedFraction
                        << " shared=" << LayeredFloors::SharedFraction(floor, transforms[i], anchorX, anchorY)
                        << (LayeredFloors::InsideWithMargin(floor, transforms[i], anchorX, anchorY, 1)
                            ? "  solid=yes" : "  solid=no")
                        << (LayeredFloors::SharesSurfaceGround(floor) ? "  shares=yes" : "  shares=no")
                        << (adjacent ? "  adjacent=yes" : "  adjacent=no") << '\n';
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



