#include "LayeredMapState.h"

#include "../Diagnostics/Diagnostics.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>

namespace LayeredMap {
namespace {

struct Entry {
    int sceneId = 0;
    // ItemDatas::layer::stateId carries the Kuro state (World = 8), NOT the runtime scene id
    // (World = 1). RoleFor compares that field, so the state has to be kept alongside.
    int kuroStateId = 0;
    std::string regionId;
    LayeredFloors::FloorEntry floor;
    LayeredFloors::Transform transform;
};

std::mutex stateMutex;
std::vector<Entry> entries;
Snapshot current;
// A floor has to win `kSwitchFrames` classifications in a row before it is adopted, and
// `kClearFrames` unknowns in a row before the state clears. Walking in and out of a cave
// otherwise flickers the entire marker set on and off.
constexpr int kSwitchFrames = 3;
// Long enough that a stretch of weak classifications cannot end a floor the player never left
// (see the footprint check in ObserveMinimap, which normally decides this on its own).
constexpr int kClearFrames = 10;
std::string pendingFloorId;
int pendingCount = 0;
int unknownCount = 0;
std::chrono::steady_clock::time_point lastClassifyAt{};
std::chrono::steady_clock::time_point lastReportAt{};
// Classification is a BFMatcher sweep over every floor's descriptors. Captures arrive
// several times a second; three per second is plenty to follow a player around.
constexpr auto kMinimumInterval = std::chrono::milliseconds(300);

std::string VoteSummary(const LayeredFloors::Classification& classification, const std::vector<Entry>& source) {
    std::string summary;
    for (std::size_t index = 0; index < classification.votes.size() && index < 4; ++index) {
        const auto& vote = classification.votes[index];
        const auto found = std::find_if(source.begin(), source.end(), [&](const Entry& entry) {
            return entry.floor.floorId == vote.floorId;
        });
        if (!summary.empty()) summary += " ";
        summary += found == source.end() ? vote.floorId : found->regionId + ":" + found->floor.floorId;
        summary += "=" + std::to_string(vote.matches);
    }
    return summary;
}

} // namespace

void Install(const std::filesystem::path& featureDataRoot) {
    std::vector<Entry> loaded;
    const auto packsRoot = featureDataRoot / "KuroTilePacks";
    std::error_code error;
    if (!std::filesystem::is_directory(packsRoot, error)) {
        Diagnostics::Record("layered-floor-index", "stage=skipped reason=no-packs-dir root=" + packsRoot.string());
        return;
    }
    for (const auto& directory : std::filesystem::directory_iterator(packsRoot, error)) {
        if (error || !directory.is_directory()) continue;
        const auto packRoot = directory.path();
        if (!std::filesystem::exists(packRoot / "layered-floors" / "floor-index.json")) continue;
        int sceneId = 0;
        int kuroStateId = 0;
        try {
            std::ifstream manifest(packRoot / "manifest.json");
            if (manifest) {
                nlohmann::json json;
                manifest >> json;
                sceneId = json.value("sceneId", 0);
                kuroStateId = json["source"].value("state", 0);
            }
        }
        catch (...) {
            sceneId = 0;
            kuroStateId = 0;
        }
        std::vector<LayeredFloors::FloorEntry> floors;
        LayeredFloors::Index index;
        std::string loadError;
        if (!LayeredFloors::Load(packRoot, index, loadError)) {
            Diagnostics::Record("layered-floor-index", "region=" + directory.path().filename().string() +
                " loaded=0 error=" + loadError);
            continue;
        }
        const auto region = packRoot.filename().string();
        for (auto& floor : index.floors) {
            loaded.push_back(Entry{sceneId, kuroStateId, region, std::move(floor), index.transform});
        }
        Diagnostics::Record("layered-floor-index", "region=" + region + " scene=" + std::to_string(sceneId) +
            " kuroState=" + std::to_string(kuroStateId) + " floors=" + std::to_string(index.floors.size()));
    }

    std::lock_guard lock(stateMutex);
    entries = std::move(loaded);
    current = Snapshot{};
    pendingFloorId.clear();
    pendingCount = 0;
    unknownCount = 0;
    Diagnostics::Record("layered-floor-index", "stage=ready floors=" + std::to_string(entries.size()));
}

void ObserveMinimap(const ImageFeatureData& minimapFeatures, int sceneId, double mapX, double mapY) {
    if (minimapFeatures.imgDescriptors.empty() || sceneId == 0) return;
    const auto now = std::chrono::steady_clock::now();

    std::vector<Entry> candidates;
    bool restricted = false;
    std::string containing;
    {
        std::lock_guard lock(stateMutex);
        if (entries.empty()) return;
        if (lastClassifyAt.time_since_epoch().count() != 0 && now - lastClassifyAt < kMinimumInterval) return;
        lastClassifyAt = now;
        // Physically impossible floors are not evidence. Standing inside 眠龙庭's cave rules
        // out 叩天关's, even though they share a tile coordinate; without this, a rival floor
        // with six matches was enough to hold the true floor under the margin.
        std::vector<const Entry*> inside;
        for (const auto& entry : entries) {
            if (entry.sceneId != sceneId) continue;
            if (LayeredFloors::Contains(entry.floor, entry.transform, mapX, mapY)) inside.push_back(&entry);
        }
        restricted = !inside.empty();
        for (const auto& entry : entries) {
            if (entry.sceneId != sceneId) continue;
            if (restricted && !LayeredFloors::Contains(entry.floor, entry.transform, mapX, mapY)) continue;
            candidates.push_back(entry);
            if (restricted) {
                if (!containing.empty()) containing += " ";
                containing += entry.floor.floorId;
            }
        }
    }
    if (candidates.empty()) return;

    std::vector<LayeredFloors::FloorEntry> floors;
    floors.reserve(candidates.size());
    for (const auto& entry : candidates) floors.push_back(entry.floor);
    // Inside a cave the only real question is which of the floors sharing it, and those votes
    // are small (4-9 on a real 眠龙庭·上层 position); out in the open the bar stays where the
    // surface references were calibrated, because there the imagery is the only evidence.
    //
    // The lower bar is safe because every floor's composite shares the same dimmed surface
    // base: on the surface above a cave the shared base matches all of them equally, so no
    // floor can lead by the 2x margin, while inside the cave the cave texture separates them.
    const int minimumMatches = restricted ? 4 : 10;
    const auto classification = LayeredFloors::Classify(minimapFeatures, floors, minimumMatches, 2.0);

    std::lock_guard lock(stateMutex);
    if (classification.identified) {
        unknownCount = 0;
        if (classification.floorId == current.floorId && current.active) {
            pendingFloorId.clear();
            pendingCount = 0;
        }
        else if (classification.floorId == pendingFloorId) {
            ++pendingCount;
        }
        else {
            pendingFloorId = classification.floorId;
            pendingCount = 1;
        }
        if (pendingCount >= kSwitchFrames && classification.floorId != current.floorId) {
            const auto found = std::find_if(candidates.begin(), candidates.end(), [&](const Entry& entry) {
                return entry.floor.floorId == classification.floorId;
            });
            current.active = true;
            current.sceneId = sceneId;
            current.kuroStateId = found == candidates.end() ? 0 : found->kuroStateId;
            current.floorId = classification.floorId;
            current.level = found == candidates.end() ? LayeredFloors::FloorLevel(classification.floorId) : found->floor.level;
            current.layerId = found == candidates.end() ? LayeredFloors::FloorLayerId(classification.floorId) : found->floor.layerId;
            ++current.revision;
            pendingFloorId.clear();
            pendingCount = 0;
            Diagnostics::Record("layered-floor-change", "scene=" + std::to_string(sceneId) +
                " region=" + (found == candidates.end() ? std::string("?") : found->regionId) +
                " floor=" + current.floorId +
                " name=" + (found == candidates.end() ? std::string("?") : found->floor.floorName) +
                " matches=" + std::to_string(classification.winnerMatches) +
                " runnerUp=" + std::to_string(classification.runnerUpMatches));
        }
    }
    else {
        pendingFloorId.clear();
        pendingCount = 0;
        ++unknownCount;
        // The imagery alone is not decisive: at some spots inside 眠龙庭·上层 the correct floor
        // scores 4-11 against a threshold of 10 while surface frames reach 6, so requiring a
        // fresh identification every few seconds made the state flicker on and off. Once a
        // floor is known, it is kept for as long as the player is standing in that floor's
        // cave; leaving the cave (or a position jump) is what ends it.
        const auto active = std::find_if(entries.begin(), entries.end(), [&](const Entry& entry) {
            return entry.floor.floorId == current.floorId && entry.sceneId == current.sceneId;
        });
        const bool stillInside = current.active && active != entries.end() &&
            LayeredFloors::Contains(active->floor, active->transform, mapX, mapY);
        if (current.active && unknownCount >= kClearFrames && !stillInside) {
            const auto previous = current.floorId;
            current = Snapshot{};
            ++current.revision;
            Diagnostics::Record("layered-floor-change", "scene=" + std::to_string(sceneId) +
                " floor=cleared previous=" + previous + " unknownFrames=" + std::to_string(unknownCount));
        }
    }

    // Periodic evidence line so a session log shows what the classifier saw, not just what
    // it decided: the vote table is what makes a wrong decision diagnosable after the fact.
    if (now - lastReportAt >= std::chrono::seconds(5)) {
        lastReportAt = now;
        Diagnostics::Record("layered-floor", "scene=" + std::to_string(sceneId) +
            " active=" + std::to_string(current.active) + " floor=" + (current.floorId.empty() ? "-" : current.floorId) +
            " restricted=" + std::to_string(restricted) + " containing=[" + containing + "]" +
            " identified=" + std::to_string(classification.identified) +
            " winner=" + std::to_string(classification.winnerMatches) +
            " runnerUp=" + std::to_string(classification.runnerUpMatches) +
            " votes=[" + VoteSummary(classification, candidates) + "]");
    }
}

Snapshot Read() {
    std::lock_guard lock(stateMutex);
    return current;
}

MarkerRole RoleFor(const ItemDatas& item) {
    const auto state = Read();
    if (!state.active) return MarkerRole::Normal;
    // The marker carries the Kuro state (World = 8); the state holds that same field. Keeping
    // the two id spaces apart is what makes this check meaningful - comparing it against the
    // runtime scene id matched nothing and silently skipped every layered rule.
    if (item.layer.stateId != 0 && state.kuroStateId != 0 && item.layer.stateId != state.kuroStateId) {
        return MarkerRole::Normal;
    }

    // The two fields mean different things, which is easy to get backwards: the point's
    // `floorId` names the LAYERED MAP ("1" = 叩天关, "48" = 元林再生舱) and its `level` names
    // the floor inside that map ("-2/1"). The classifier reports the floor id.
    const auto& mapId = item.layer.floorId;
    const auto& floorId = item.layer.level;
    if (mapId.empty() || floorId.empty()) return MarkerRole::Hidden; // plain surface collectible
    const int level = LayeredFloors::FloorLevel(floorId);
    // An entrance marker ("-1000000/1") and a floor-less point ("0") sit on the surface.
    if (level == 0 || level <= -1000000) return MarkerRole::Hidden;
    if (floorId == state.floorId) return MarkerRole::Current;
    if (LayeredFloors::FloorLayerId(floorId) != state.layerId) return MarkerRole::Hidden; // another layered map
    // More negative is deeper: "-1" is the top floor, "-3" the bottom.
    return level < state.level ? MarkerRole::Below : MarkerRole::Above;
}

void Reset() {
    std::lock_guard lock(stateMutex);
    entries.clear();
    current = Snapshot{};
    pendingFloorId.clear();
    pendingCount = 0;
    unknownCount = 0;
    lastClassifyAt = std::chrono::steady_clock::time_point{};
    lastReportAt = std::chrono::steady_clock::time_point{};
}

void SetForTest(const Snapshot& snapshot) {
    std::lock_guard lock(stateMutex);
    current = snapshot;
}

} // namespace LayeredMap
