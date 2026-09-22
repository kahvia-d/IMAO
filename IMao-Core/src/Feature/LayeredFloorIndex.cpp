#include "LayeredFloorIndex.h"

#include "Processing/FeatureBinaryCodec.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

using json = nlohmann::json;

namespace LayeredFloors {
namespace {

std::string ReadString(const json& node, const char* key) {
    const auto it = node.find(key);
    return it == node.end() || it->is_null() ? std::string() : it->get<std::string>();
}

} // namespace

int FloorLevel(const std::string& floorId) {
    const auto slash = floorId.find('/');
    if (slash == std::string::npos || slash == 0) return 0;
    try {
        return std::stoi(floorId.substr(0, slash));
    }
    catch (...) {
        return 0;
    }
}

int FloorLayerId(const std::string& floorId) {
    const auto slash = floorId.find('/');
    if (slash == std::string::npos || slash + 1 >= floorId.size()) return 0;
    try {
        return std::stoi(floorId.substr(slash + 1));
    }
    catch (...) {
        return 0;
    }
}

bool Load(const std::filesystem::path& packDirectory, std::vector<FloorEntry>& floors, std::string& error) {
    floors.clear();
    const auto indexPath = packDirectory / "layered-floors" / "floor-index.json";
    if (!std::filesystem::exists(indexPath)) {
        error = "no layered floor index at " + indexPath.string();
        return false;
    }
    std::ifstream input(indexPath);
    if (!input) {
        error = "cannot open " + indexPath.string();
        return false;
    }
    json index;
    try {
        input >> index;
    }
    catch (const std::exception& exception) {
        error = "cannot parse " + indexPath.string() + ": " + exception.what();
        return false;
    }
    if (index.value("formatVersion", 0) != 1) {
        error = "unsupported layered floor index format";
        return false;
    }
    const auto entries = index.find("floors");
    if (entries == index.end() || !entries->is_array()) {
        error = "layered floor index has no floors array";
        return false;
    }
    const auto root = indexPath.parent_path();
    for (const auto& node : *entries) {
        FloorEntry entry;
        entry.layerId = node.value("layerId", 0);
        entry.floorId = ReadString(node, "floorId");
        entry.layerName = ReadString(node, "layerName");
        entry.floorName = ReadString(node, "floorName");
        entry.level = FloorLevel(entry.floorId);
        const auto file = ReadString(node, "file");
        if (entry.floorId.empty() || file.empty()) {
            error = "layered floor index entry is missing floorId or file";
            return false;
        }
        std::string loadError;
        if (!FeatureBinaryCodec::Load(root / file, entry.features, loadError)) {
            error = "cannot load " + (root / file).string() + ": " + loadError;
            return false;
        }
        if (entry.features.imgKeypoints.empty() ||
            entry.features.imgDescriptors.rows != static_cast<int>(entry.features.imgKeypoints.size())) {
            error = "layered floor " + entry.floorId + " has an invalid feature set";
            return false;
        }
        floors.push_back(std::move(entry));
    }
    if (floors.empty()) {
        error = "layered floor index lists no floors";
        return false;
    }
    return true;
}

Classification Classify(const ImageFeatureData& query, const std::vector<FloorEntry>& floors,
    int minimumMatches, double margin, float ratio, float maxDistance) {
    Classification result;
    if (query.imgDescriptors.empty() || query.imgDescriptors.rows < 2 || floors.empty()) return result;

    cv::BFMatcher matcher(cv::NORM_L2);
    for (const auto& floor : floors) {
        if (floor.features.imgDescriptors.empty()) continue;
        std::vector<std::vector<cv::DMatch>> knn;
        matcher.knnMatch(query.imgDescriptors, floor.features.imgDescriptors, knn, 2);
        FloorVote vote;
        vote.layerId = floor.layerId;
        vote.floorId = floor.floorId;
        for (const auto& pair : knn) {
            if (pair.size() < 2) continue;
            // Same test the localizer uses: nearest neighbour must beat the second by a ratio
            // and stay inside an absolute distance.
            if (pair[0].distance < ratio * pair[1].distance && pair[0].distance < maxDistance) ++vote.matches;
        }
        result.votes.push_back(std::move(vote));
    }
    std::sort(result.votes.begin(), result.votes.end(),
        [](const FloorVote& left, const FloorVote& right) {
            if (left.matches != right.matches) return left.matches > right.matches;
            return left.floorId < right.floorId;
        });
    if (result.votes.empty()) return result;

    result.winnerMatches = result.votes.front().matches;
    result.runnerUpMatches = result.votes.size() > 1 ? result.votes[1].matches : 0;
    result.layerId = result.votes.front().layerId;
    result.floorId = result.votes.front().floorId;
    const bool enough = result.winnerMatches >= minimumMatches;
    const bool leads = result.runnerUpMatches == 0 ||
        static_cast<double>(result.winnerMatches) >= margin * static_cast<double>(result.runnerUpMatches);
    result.identified = enough && leads;
    return result;
}

} // namespace LayeredFloors
