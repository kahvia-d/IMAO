#pragma once
#include "../Domain/MapData.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace NearbySelection {
using Clock = std::chrono::steady_clock;
enum class Intent { Complete, Guide };
inline constexpr double CompletionPixels = 15.0;
inline constexpr double GuideMapDistance = 120.0;
struct Candidate { ItemDatas item; double distance = 0, screenDistance = 0; };
struct Observation {
    bool available = false;
    std::uint64_t session = 0, gameHwnd = 0, filterRevision = 0;
    std::uint32_t gameProcessId = 0;
    std::string profileId, sceneName;
    Clock::time_point locatedAt{}, freshUntil{};
    std::vector<Candidate> candidates;
};
inline std::string Key(const ItemDatas& item) { return std::to_string(item.layer.stateId) + ":" + item.itemId; }
inline bool Includes(const Candidate& item, Intent intent) {
    return intent == Intent::Guide || item.screenDistance < CompletionPixels;
}
inline std::vector<Candidate> Collect(const ItemMarkerFrame& frame, const Coordinate& playerROC,
    double pixelsPerMapUnit = 0) {
    std::vector<Candidate> result;
    std::unordered_set<std::string> seen;
    if (!std::isfinite(playerROC.x) || !std::isfinite(playerROC.y) || frame.radius <= 0) return result;
    for (const auto& item : frame.markers) {
        const double distance = std::hypot(item.itemMapROC.x - playerROC.x, item.itemMapROC.y - playerROC.y);
        const double pixels = pixelsPerMapUnit > 0 ? distance * pixelsPerMapUnit :
            std::hypot(item.screenCoordiante.x - frame.center.x, item.screenCoordiante.y - frame.center.y);
        if (!item.isSaved && item.layer.stateId > 0 && !item.itemId.empty() && std::isfinite(distance) &&
            std::isfinite(pixels) && distance <= GuideMapDistance && pixels <= frame.radius && seen.insert(Key(item)).second)
            result.push_back({item, distance, pixels});
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        if (a.distance != b.distance) return a.distance < b.distance;
        if (a.item.layer.stateId != b.item.layer.stateId) return a.item.layer.stateId < b.item.layer.stateId;
        return a.item.itemId < b.item.itemId;
    });
    return result;
}

// The list may stay open while fresh position observations replace each other.
// Its original point identities, account, game process and filter remain fixed.
struct Session {
    Observation source;
    Intent intent = Intent::Complete;
    std::uint64_t revision = 0, chooserHwnd = 0, chooserGeneration = 0, registrationRevision = 0;
    bool consumed = false;
    std::string consumedKey;
    std::string Validate(const Observation& current, std::uint64_t currentFilter,
        std::uint64_t requestedRevision, const std::string& profile, const std::string& scene,
        const std::string& key, Clock::time_point now = Clock::now()) const {
        if (revision != requestedRevision || profile != source.profileId || scene != source.sceneName) return "selection-expired";
        if (current.session != source.session || current.gameHwnd != source.gameHwnd ||
            current.gameProcessId != source.gameProcessId || current.profileId != profile || current.sceneName != scene)
            return "nearby-context-changed";
        if (currentFilter != source.filterRevision || current.filterRevision != currentFilter) return "nearby-filter-changed";
        if (!current.available || now < current.locatedAt || now - current.locatedAt > std::chrono::seconds(1) ||
            now >= current.freshUntil) return "nearby-position-unavailable";
        const auto matches = [&](const Candidate& item) { return !item.item.isSaved && Key(item.item) == key && Includes(item, intent); };
        if (std::none_of(source.candidates.begin(), source.candidates.end(), matches)) return "not-a-selection-candidate";
        if (std::none_of(current.candidates.begin(), current.candidates.end(), matches)) return "nearby-point-no-longer-eligible";
        if (consumed && key != consumedKey) return "selection-already-consumed";
        return {};
    }
};

inline std::string ValidateSingleCompletion(const Observation& source, const Observation& current,
    std::uint64_t currentFilter, Clock::time_point now = Clock::now()) {
    const auto eligible = [](const Candidate& item) { return !item.item.isSaved && Includes(item, Intent::Complete); };
    if (std::count_if(source.candidates.begin(), source.candidates.end(), eligible) != 1)
        return "nearby-single-selection-changed";
    const auto selected = std::find_if(source.candidates.begin(), source.candidates.end(), eligible);
    const Session selection{source, Intent::Complete, 1};
    const auto failure = selection.Validate(current, currentFilter, 1, source.profileId, source.sceneName, Key(selected->item), now);
    if (!failure.empty()) return failure;
    if (std::count_if(current.candidates.begin(), current.candidates.end(), eligible) != 1)
        return "nearby-single-selection-changed";
    return {};
}
}
