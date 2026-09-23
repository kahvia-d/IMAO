#pragma once
#include "../Domain/MapData.h"
#include "LayeredMapState.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace NearbySelection {
using Clock = std::chrono::steady_clock;
enum class Intent { Complete, Guide };
// The key acts on the nearest eligible point inside the player's own range, and the
// range is the one part of that rule a player can move: 15 pixels is what the tool
// always used, and the two keys measure their own distance from the player arrow.
inline constexpr int DefaultRangePixels = 15;
inline constexpr int MinimumRangePixels = 5;
inline constexpr int MaximumRangePixels = 120;
// How far a guide candidate may sit from the player in map units at all; the pixel
// range above is what decides whether the key actually acts on it.
inline constexpr double GuideMapDistance = 120.0;

// One packed atomic publishes both ranges together to the polling, IPC and overlay
// threads, exactly like the hotkey bindings.
class Ranges {
public:
    static int Completion() { return static_cast<int>(packed_.load() & 0xFFFFu); }
    static int Guide() { return static_cast<int>((packed_.load() >> 16) & 0xFFFFu); }
    static double Pixels(Intent intent) { return intent == Intent::Guide ? Guide() : Completion(); }
    static void Validate(int completion, int guide) {
        if (completion < MinimumRangePixels || completion > MaximumRangePixels ||
            guide < MinimumRangePixels || guide > MaximumRangePixels)
            throw std::invalid_argument("触发范围必须在 5–120 像素之间");
    }
    static void Apply(int completion, int guide) {
        Validate(completion, guide);
        packed_.store(static_cast<std::uint64_t>(completion) | (static_cast<std::uint64_t>(guide) << 16));
    }
private:
    inline static std::atomic<std::uint64_t> packed_{static_cast<std::uint64_t>(DefaultRangePixels) |
        (static_cast<std::uint64_t>(DefaultRangePixels) << 16)};
};

struct Candidate { ItemDatas item; double distance = 0, screenDistance = 0; };
struct Observation {
    bool available = false;
    std::uint64_t session = 0, gameHwnd = 0, filterRevision = 0;
    std::uint32_t gameProcessId = 0;
    std::string profileId, sceneName;
    Clock::time_point locatedAt{}, freshUntil{};
    std::vector<Candidate> candidates;
    // Drawn marker radius of the observed frame; zero when it was never drawn.
    double markerRadius = 0;
};
inline std::string Key(const ItemDatas& item) { return std::to_string(item.layer.stateId) + ":" + item.itemId; }
inline bool Includes(const Candidate& item, Intent intent) {
    // A marker the display is hiding must not be reachable by a key press either. Standing in a
    // layered map hides every surface point, and without this the completion key would still tick
    // one of them off - the key acts on what the player can see.
    const auto role = LayeredMap::RoleFor(item.item);
    if (role == LayeredMap::MarkerRole::Hidden) return false;
    // Completing is floor-exact: standing on 1楼 must not tick off the chest one floor up or down,
    // because the player cannot have reached it. Reading a guide is not floor-exact, so those
    // floors stay eligible for the guide intent.
    if (intent == Intent::Complete &&
        (role == LayeredMap::MarkerRole::Above || role == LayeredMap::MarkerRole::Below)) return false;
    return !item.item.isSaved && std::isfinite(item.screenDistance) && item.screenDistance < Ranges::Pixels(intent);
}

// The nearest eligible point, with the tie-break the candidate list is sorted by.
inline const Candidate* Nearest(const std::vector<Candidate>& candidates) {
    const Candidate* best = nullptr;
    for (const auto& item : candidates)
        if (best == nullptr || item.distance < best->distance ||
            (item.distance == best->distance && Key(item.item) < Key(best->item))) best = &item;
    return best;
}

// The points whose icons overlap the anchor's icon. `markerDiameter` is the drawn
// icon diameter plus the layout gap, so this is exactly the set the minimap stacks
// under one badge — and it is a bounded group, never a chain across the minimap.
// An unknown diameter (zero) keeps every candidate: a caller that cannot prove two
// icons are apart must ask the player, never pick one of them.
inline std::vector<Candidate> OverlapGroup(const std::vector<Candidate>& candidates, const Candidate& anchor,
    double markerDiameter) {
    if (!(markerDiameter > 0) || !std::isfinite(markerDiameter)) return candidates;
    std::vector<Candidate> group;
    for (const auto& item : candidates)
        if (std::hypot(item.item.screenCoordiante.x - anchor.item.screenCoordiante.x,
            item.item.screenCoordiante.y - anchor.item.screenCoordiante.y) <= markerDiameter) group.push_back(item);
    return group;
}

inline std::vector<Candidate> Eligible(const std::vector<Candidate>& candidates, Intent intent) {
    std::vector<Candidate> result;
    for (const auto& item : candidates) if (Includes(item, intent)) result.push_back(item);
    return result;
}

// The diameter the minimap stacks icons by (drawn radius * 2 plus the layout gap), or
// zero when the frame was never drawn — which makes the overlap group every candidate.
inline double OverlapDiameter(const Observation& observation) {
    return observation.markerRadius > 0 ? observation.markerRadius * 2 + 2 : 0.0;
}

// The nearby rule, in one place: the nearest eligible point inside the player's
// range, or that point's whole icon-overlap group when it is not alone. Returns an
// empty group when nothing is in range.
inline std::vector<Candidate> Resolve(const std::vector<Candidate>& candidates, Intent intent, double markerDiameter) {
    auto eligible = Eligible(candidates, intent);
    const auto* nearest = Nearest(eligible);
    if (nearest == nullptr) return {};
    return OverlapGroup(eligible, *nearest, markerDiameter);
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
        // One submission does not consume the list: a player completing several nearby
        // points in a row validates every one of them on its own. Idempotence is kept
        // per point by the caller's saved results, not by refusing every later point.
        return {};
    }
};

// Re-checks, under the caller's operation lock, that the point a key press resolved
// is still the one a fresh observation resolves. Refresh durable completion before
// calling this: the arrow moving on, another icon starting to overlap the chosen one,
// a changed filter, account, scene or expired position all refuse the write rather
// than inherit the original choice.
inline std::string ValidateSingleSelection(const Observation& source, const Observation& current,
    std::uint64_t currentFilter, Intent intent, Clock::time_point now = Clock::now()) {
    const auto resolved = Resolve(source.candidates, intent, OverlapDiameter(source));
    if (resolved.size() != 1) return "nearby-single-selection-changed";
    const auto selected = resolved.front();
    const Session selection{source, intent, 1};
    const auto failure = selection.Validate(current, currentFilter, 1, source.profileId, source.sceneName,
        Key(selected.item), now);
    if (!failure.empty()) return failure;
    const auto currentResolved = Resolve(current.candidates, intent, OverlapDiameter(current));
    if (currentResolved.size() != 1) return "nearby-single-selection-changed";
    if (Key(currentResolved.front().item) != Key(selected.item)) return "nearby-point-no-longer-nearest";
    return {};
}
}
