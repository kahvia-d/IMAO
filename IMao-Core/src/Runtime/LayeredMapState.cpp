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
Scope scope;
// A floor has to win `kSwitchFrames` classifications in a row before it is adopted, and
// `kClearFrames` unknowns in a row before the state clears. Walking in and out of a cave
// otherwise flickers the entire marker set on and off.
constexpr int kSwitchFrames = 3;
// Long enough that a stretch of weak classifications cannot end a floor the player never left
// (see the footprint check in ObserveMinimap, which normally decides this on its own).
constexpr int kClearFrames = 10;
// How long the player may stand outside the known floor's footprint before it is dropped. This is
// the position-only test, so it also runs while no minimap is being captured at all, and it is
// counted in TIME rather than classifications: those arrive every few seconds in practice, not
// every 300 ms, which is why ten of them meant tens of seconds of a stale floor.
constexpr auto kLeftFootprintFor = std::chrono::milliseconds(2500);
std::chrono::steady_clock::time_point outsideFootprintSince{};
// A near-tie among the floors the player can be in - 下层金库's 贵金属与艺术品藏区 votes 12/10/5/4
// for four floors of the same marble hall - is still a decision, but it takes this many matches to
// make: the winner is the only candidate too often for a small count to mean anything.
constexpr int kNearTieMatches = 10;
std::string pendingFloorId;
// Consecutive decisive classifications; see kGroupResetFrames.
int decisiveStreak = 0;
// Consecutive close classifications; see kGroupGrowFrames.
int closeStreak = 0;
// Consecutive frames of shared-ground evidence; see kSharedGroundFraction. Signed: it counts up
// towards "shared" and down towards "not shared", so the verdict has the same hysteresis leaving
// the plaza as entering it.
int sharedGroundFrames = 0;
// The debounced verdict, and the last one that was logged, so the diagnostic is one line per
// change instead of one per second.
bool sharedGround = false;
bool sharedGroundLogged = false;
int pendingCount = 0;
int unknownCount = 0;
std::chrono::steady_clock::time_point lastClassifyAt{};
std::chrono::steady_clock::time_point lastReportAt{};
// Classification is a BFMatcher sweep over every floor's descriptors. Captures arrive
// several times a second; three per second is plenty to follow a player around.
constexpr auto kMinimumInterval = std::chrono::milliseconds(300);
// Out in the open the classifier is only waiting to notice that the player walked into a cave,
// and that answer does not change within a second. Every floor in the game is a candidate, so
// the cheaper cadence matters: three times a second is kept for the cases that are actually
// tracking a layered floor, or have no position yet and are using the vote as a search scope.
constexpr auto kIdleInterval = std::chrono::milliseconds(1000);

// Two floors count as the same place when their votes are within this factor. Inside 下层金库's
// 贵金属与艺术品藏区 the votes can be near-tied (12/10), and then the display must say "you are
// on one of these" instead of picking one and calling the others above or below - that is what
// stops the state flickering between them.
//
// It used to be 3, which was calibrated while the deciding vote ran on a 600-descriptor sample
// that could not separate the floors at all. With the full set the same captures lead 17 vs 8 and
// 15 vs 6, and a factor of 3 swallowed those real leads too: every floor showed as the current
// one, which is what the user saw. 1.5 keeps the near-tie case and lets a 2x lead mean something.
constexpr int kIndistinguishableFactor = 1.5;   // vote * 1.5 >= winner  <=>  vote >= winner / 1.5

// How many consecutive decisive classifications it takes before the equivalence group collapses
// back to the winner. Long enough that a burst of close frames cannot shrink it mid-tie (which
// would flicker the other floors' markers), short enough to stay under two seconds.
constexpr int kGroupResetFrames = 5;

// And how many consecutive close classifications it takes before a floor joins the group. The
// votes have two regimes - a clear frame reads 14 vs 4 while one against a blank wall reads
// 1 vs 1 - so a single weak frame must not decide that two floors are the same place.
constexpr int kGroupGrowFrames = 3;

// A layered map may reuse part of the surface as its own ground. Where this much of the art
// around the player is the surface's own pixels, the player is on ground both maps describe.
// Measured on 下层金库 with radius 3: the shared plaza reads 0.67-0.69, the hall inside the
// building 0.16-0.34, so 0.5 sits between them with roughly a 2x margin.
constexpr double kSharedGroundFraction = 0.5;
// Frames of that evidence before the verdict flips: the fraction moves as the player walks the
// boundary between the plaza and the building.
constexpr int kSharedGroundFrames = 3;

// Descriptors kept per floor when the index is loaded. The whole game has 90 floors and a cold
// start compares every one of them, because the question is "which floor", not "where". Measured
// against both real in-cave frames with all 90 floors on the table: keeping every descriptor gave
// 22 vs 7 and 22 vs 5, 600 gave 26 vs 12 and 20 vs 7, and 300 broke identification outright
// (22 vs 14, and a 9 vs 8 wrong answer), so 600 is the smallest cap that still separates them.
constexpr int kFloorDescriptorCap = 600;

std::string JoinFloors(const std::vector<std::string>& floors) {
    std::string joined;
    for (const auto& floor : floors) {
        if (!joined.empty()) joined += " ";
        joined += floor;
    }
    return joined;
}

std::string VoteSummary(const LayeredFloors::Classification& classification, const std::vector<const Entry*>& source) {
    std::string summary;
    for (std::size_t index = 0; index < classification.votes.size() && index < 4; ++index) {
        const auto& vote = classification.votes[index];
        // The vote carries where it came from: with every region on the table the same floor id
        // can appear more than once, so looking the id up again would report the wrong region.
        const Entry* entry = vote.sourceIndex < source.size() ? source[vote.sourceIndex] : nullptr;
        if (!summary.empty()) summary += " ";
        summary += entry == nullptr ? vote.floorId : entry->regionId + ":" + entry->floor.floorId;
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
        if (!LayeredFloors::Load(packRoot, index, loadError, kFloorDescriptorCap)) {
            Diagnostics::Record("layered-floor-index", "region=" + directory.path().filename().string() +
                " loaded=0 error=" + loadError);
            continue;
        }
        const auto region = packRoot.filename().string();
        for (auto& floor : index.floors) {
            loaded.push_back(Entry{sceneId, kuroStateId, region, std::move(floor), index.transform});
        }
        Diagnostics::Record("layered-floor-index", "region=" + region + " scene=" + std::to_string(sceneId) +
            " kuroState=" + std::to_string(kuroStateId) + " floors=" + std::to_string(index.floors.size()) +
            " descriptorCap=" + std::to_string(kFloorDescriptorCap));
    }

    std::lock_guard lock(stateMutex);
    entries = std::move(loaded);
    current = Snapshot{};
    pendingFloorId.clear();
    pendingCount = 0;
    unknownCount = 0;
    sharedGroundFrames = 0;
    sharedGround = false;
    sharedGroundLogged = false;
    outsideFootprintSince = std::chrono::steady_clock::time_point{};
    Diagnostics::Record("layered-floor-index", "stage=ready floors=" + std::to_string(entries.size()));
}

void ObserveMinimap(const ImageFeatureData& minimapFeatures, int sceneId, double mapX, double mapY) {
    if (minimapFeatures.imgDescriptors.empty()) return;
    const auto now = std::chrono::steady_clock::now();

    // Cold start: no scene is known, so no position can be interpreted either. The minimap can
    // still say which floor's cave it is looking at, and that floor's footprint is somewhere to
    // point the localizer's sweep. Nothing is published from this - it is a search scope only.
    if (sceneId == 0) {
        // Pointers, not copies: a FloorEntry owns its descriptors, and once every floor in the
        // game is a candidate that copy was tens of megabytes per frame. `entries` is written
        // once by Install and never mutated afterwards, so referring into it is safe.
        std::vector<const Entry*> all;
        {
            std::lock_guard lock(stateMutex);
            if (entries.empty()) return;
            if (lastClassifyAt.time_since_epoch().count() != 0 && now - lastClassifyAt < kMinimumInterval) return;
            lastClassifyAt = now;
            all.reserve(entries.size());
            for (const auto& entry : entries) all.push_back(&entry);
        }
        std::vector<const LayeredFloors::FloorEntry*> floors;
        floors.reserve(all.size());
        for (const auto* entry : all) floors.push_back(&entry->floor);
        // The scope only has to pick the right cave, not confirm a position: a wrong guess
        // costs one bounded search that falls back to the global sweep anyway.
        // Every floor in the game is a candidate here and the answer only scopes a search, so the
        // capped sample is enough - see FloorEntry::sampleFeatures.
        const auto classification = LayeredFloors::Classify(minimapFeatures, floors, 4, 1.5, 0.75f, 0.6f, true);
        std::lock_guard lock(stateMutex);
        if (!classification.identified) return;
        const auto found = std::find_if(all.begin(), all.end(), [&](const Entry* entry) {
            return entry->floor.floorId == classification.floorId;
        });
        if (found == all.end() || !(*found)->floor.hasCenter) return;
        const auto& scopeEntry = **found;
        if (scope.valid && scope.floorId == classification.floorId && scope.sceneId == scopeEntry.sceneId) return;
        scope.valid = true;
        scope.sceneId = scopeEntry.sceneId;
        scope.layerId = scopeEntry.floor.layerId;
        scope.floorId = scopeEntry.floor.floorId;
        scope.mapX = scopeEntry.floor.centerMapX;
        scope.mapY = scopeEntry.floor.centerMapY;
        Diagnostics::Record("layered-floor-scope", "scene=" + std::to_string(scope.sceneId) +
            " region=" + scopeEntry.regionId + " floor=" + scope.floorId + " name=" + scopeEntry.floor.floorName +
            " center=" + std::to_string(scope.mapX) + "," + std::to_string(scope.mapY) +
            " matches=" + std::to_string(classification.winnerMatches) +
            " runnerUp=" + std::to_string(classification.runnerUpMatches));
        return;
    }

    std::vector<const Entry*> candidates;
    // The floors the player's position can physically be in; the deciding vote below needs them
    // outside the lock as well.
    std::vector<const Entry*> inside;
    bool restricted = false;
    std::string containing;
    {
        std::lock_guard lock(stateMutex);
        // A scene is known again, so the cold-start scope has done its job.
        if (scope.valid) scope = Scope{};
        if (entries.empty()) return;
        const auto interval = current.active ? kMinimumInterval : kIdleInterval;
        if (lastClassifyAt.time_since_epoch().count() != 0 && now - lastClassifyAt < interval) return;
        lastClassifyAt = now;
        // Every floor of the scene is a candidate, deliberately: the SPREAD between the floors is
        // itself the evidence that the player is in a cave at all. A surface frame taken over a
        // cave matches every one of its floors almost equally - the field log has 黯原's 虚妄摇篮 at
        // 21/21/18 from the field above it - while inside the cave the cave texture separates them.
        // Comparing only the floors that contain the player threw that away: one candidate always
        // wins its own vote, so a 4-match field frame read as a decision and held the cave for 22 s
        // while the player walked away from it. Containment is still what says which floor the
        // player can physically be standing in - see the adoption test below.
        for (const auto& entry : entries) {
            if (entry.sceneId != sceneId) continue;
            candidates.push_back(&entry);
            if (LayeredFloors::Contains(entry.floor, entry.transform, mapX, mapY)) inside.push_back(&entry);
        }
        restricted = !inside.empty();
        // A shared piece of ground belongs to both maps: 下层金库's 贵金属与艺术品藏区 draws the
        // plaza in front of the building by copying the surface pixels, so a player standing there
        // matches the floor's imagery and sits inside its footprint, yet the surface markers
        // around them (measured: one three units away) belong on screen. Nothing here can tell
        // which map the player means, so both are shown: the floor keeps its markers and the
        // surface's come back (MarkerRole::Normal), instead of guessing one and hiding the other.
        //
        // Where the layer drew its ground FROM the surface drawing, or where it sits above or
        // below the surface, the comparison says nothing: a floor only shares the surface's ground
        // if it is the one next to the surface AND its art is not a redrawing of the surface's own
        // (see LayeredFloors::SharesSurfaceGround - 星炬学院's 广场区 is the case that proved both
        // halves are needed, since its 73%-copied art made a plaza of the academy's own interior).
        bool onSharedGround = false;
        double sharedFraction = 0.0;
        const Entry* sharedEntry = nullptr;
        for (const auto& entry : entries) {
            if (entry.sceneId != sceneId) continue;
            if (!LayeredFloors::SharesSurfaceGround(entry.floor)) continue;
            const double fraction = LayeredFloors::SharedFraction(entry.floor, entry.transform, mapX, mapY);
            if (fraction < kSharedGroundFraction) continue;
            onSharedGround = true;
            sharedFraction = fraction;
            sharedEntry = &entry;
            break;
        }
        sharedGroundFrames += onSharedGround ? 1 : -1;
        if (sharedGroundFrames > kSharedGroundFrames) sharedGroundFrames = kSharedGroundFrames;
        if (sharedGroundFrames < -kSharedGroundFrames) sharedGroundFrames = -kSharedGroundFrames;
        sharedGround = sharedGroundFrames >= kSharedGroundFrames;
        if (sharedGround != sharedGroundLogged) {
            sharedGroundLogged = sharedGround;
            Diagnostics::Record("layered-floor-shared", "scene=" + std::to_string(sceneId) +
                " shared=" + std::to_string(sharedGround) +
                " fraction=" + std::to_string(sharedFraction) +
                " floor=" + (sharedEntry == nullptr ? current.floorId : sharedEntry->floor.floorId) +
                " region=" + (sharedEntry == nullptr ? std::string("?") : sharedEntry->regionId) +
                " copiedFraction=" + std::to_string(sharedEntry == nullptr ? 0.0 : sharedEntry->floor.copiedFraction));
        }
        // Carried on the snapshot so the marker roles can see it; a stale flag is impossible
        // because the assignment happens before the floor is adopted below, in the same call.
        current.sharedGround = sharedGround;
        for (const auto* entry : inside) {
            if (!containing.empty()) containing += " ";
            containing += entry->floor.floorId;
        }
    }
    if (candidates.empty()) return;

    std::vector<const LayeredFloors::FloorEntry*> floors;
    floors.reserve(candidates.size());
    for (const auto* entry : candidates) floors.push_back(&entry->floor);
    // Full descriptors, and every floor of the scene on the table: this vote decides both whether
    // the frame is a cave frame at all (a 2x lead) and which floor it is. A low absolute bar is
    // safe here because the margin carries the meaning: in-cave winners can be small (4-9 on a
    // real 眠龙庭·上层 position) while a surface frame ties every floor of the cave it is above.
    const auto classification = LayeredFloors::Classify(minimapFeatures, floors, 4, 2.0);

    const auto contained = [&](const std::string& floorId) {
        return std::any_of(inside.begin(), inside.end(), [&](const Entry* entry) {
            return entry->floor.floorId == floorId;
        });
    };
    // The floor the imagery names has to be one the player can physically be standing in: a cave
    // whose art the position is outside of is the cave BELOW the player, not the one they are in.
    const bool winnerContained = contained(classification.floorId);
    // A near-tie is no longer a decision here - the 2x lead is what says the frame is a cave frame -
    // but 下层金库's 贵金属与艺术品藏区 is four floors of one marble hall voting 12/10/5/4, where the
    // lead never comes. Such a frame still counts when the tie is between floors that share the
    // player's position and there is a real match count behind it: "the only candidate won" is not
    // evidence, and on the surface above a cave the tie is between the cave's floors and the one
    // the position points at (the field log's 21/21/18 in 入口 while standing in 一层).
    std::vector<std::string> equivalent;
    bool adopted = classification.identified && winnerContained;
    if (!adopted && restricted && winnerContained && classification.winnerMatches >= kNearTieMatches) {
        const bool tieIsLocal = std::all_of(classification.votes.begin(), classification.votes.end(),
            [&](const LayeredFloors::FloorVote& vote) {
                return vote.matches == 0 ||
                    vote.matches * kIndistinguishableFactor < classification.winnerMatches ||
                    contained(vote.floorId);
            });
        if (tieIsLocal) adopted = true;
    }
    if (adopted && restricted) {
        for (const auto& vote : classification.votes) {
            if (vote.matches == 0 ||
                vote.matches * kIndistinguishableFactor < classification.winnerMatches) continue;
            if (contained(vote.floorId)) equivalent.push_back(vote.floorId);
        }
    }

    std::lock_guard lock(stateMutex);
    if (adopted) {
        unknownCount = 0;
        // The imagery confirmed a floor, so the rim clock starts over.
        const bool sameFloor = current.active && classification.floorId == current.floorId;
        // A winner that is already in the equivalence set is another candidate for where the
        // player stands, not a floor change. 下层金库's four marble floors trade the lead from
        // frame to frame (9/4, 6/5, 12/5), and treating that as a change flipped every marker's
        // role several times a second.
        const bool equivalentToCurrent = current.active &&
            (std::find(current.equivalentFloorIds.begin(), current.equivalentFloorIds.end(),
                classification.floorId) != current.equivalentFloorIds.end() ||
             // Symmetric test: this frame cannot separate the winner from the floor already held,
             // so holding is right even when the winner changes. Without it the state flipped
             // A -> B -> A every three frames, which is the flicker the user saw.
             std::any_of(equivalent.begin(), equivalent.end(), [&](const std::string& floor) {
                 return floor == current.floorId ||
                     std::find(current.equivalentFloorIds.begin(), current.equivalentFloorIds.end(), floor) !=
                         current.equivalentFloorIds.end();
             }));
        if (sameFloor || equivalentToCurrent) {
            pendingFloorId.clear();
            pendingCount = 0;
            // The group exists to stop the display flickering while the votes are close (12/10),
            // not to keep floors lumped together once a clear lead says otherwise. Both changes
            // are debounced, because the votes have two regimes: a clear frame reads 14 vs 4,
            // while one taken against a blank wall reads 1 vs 1 and would otherwise pull its two
            // floors into the group on its own - the "everything is one layer" the user saw now
            // and then. Growing therefore needs kGroupGrowFrames close frames in a row, and
            // shrinking needs kGroupResetFrames decisive ones.
            const bool decisive = classification.winnerMatches >=
                2 * std::max(classification.runnerUpMatches, 1) && classification.winnerMatches >= 4;
            bool applyGroup = false;
            if (decisive) {
                // Decrement rather than reset the other counter: the two regimes alternate (the log
                // shows 7 vs 1, then 4 vs 3, then 14 vs 4), so a strict run of five decisive frames
                // never happened and the group stayed open for minutes. A close frame now only
                // delays the collapse instead of cancelling it.
                closeStreak = std::max(0, closeStreak - 1);
                if (++decisiveStreak >= kGroupResetFrames) {
                    current.equivalentFloorIds = equivalent;
                    decisiveStreak = 0;
                    applyGroup = false;   // the collapsed set is already the group
                }
            }
            else {
                decisiveStreak = std::max(0, decisiveStreak - 1);
                if (++closeStreak >= kGroupGrowFrames) {
                    closeStreak = 0;
                    applyGroup = true;
                }
            }
            // Merge instead of replace: the set comes from noisy votes, so a frame that separates
            // them once must not shrink the group back and make the display flip again.
            if (applyGroup) {
                for (const auto& floor : equivalent) {
                    if (std::find(current.equivalentFloorIds.begin(), current.equivalentFloorIds.end(), floor) ==
                        current.equivalentFloorIds.end()) current.equivalentFloorIds.push_back(floor);
                }
            }
            if (std::find(current.equivalentFloorIds.begin(), current.equivalentFloorIds.end(),
                current.floorId) == current.equivalentFloorIds.end()) {
                current.equivalentFloorIds.push_back(current.floorId);
            }
        }
        else if (classification.floorId == pendingFloorId) {
            ++pendingCount;
        }
        else {
            pendingFloorId = classification.floorId;
            pendingCount = 1;
        }
        if (pendingCount >= kSwitchFrames && !sameFloor && !equivalentToCurrent) {
            const auto found = std::find_if(candidates.begin(), candidates.end(), [&](const Entry* entry) {
                return entry->floor.floorId == classification.floorId;
            });
            const Entry* entry = found == candidates.end() ? nullptr : *found;
            current.active = true;
            current.sceneId = sceneId;
            current.kuroStateId = entry == nullptr ? 0 : entry->kuroStateId;
            current.floorId = classification.floorId;
            current.level = entry == nullptr ? LayeredFloors::FloorLevel(classification.floorId) : entry->floor.level;
            current.layerId = entry == nullptr ? LayeredFloors::FloorLayerId(classification.floorId) : entry->floor.layerId;
            current.heightDirection = entry == nullptr ? 1 : entry->floor.heightDirection;
            current.heightRank = entry == nullptr ? 0 : entry->floor.heightRank;
            current.equivalentFloorIds = equivalent;
            ++current.revision;
            pendingFloorId.clear();
            pendingCount = 0;
            Diagnostics::Record("layered-floor-change", "scene=" + std::to_string(sceneId) +
                " region=" + (entry == nullptr ? std::string("?") : entry->regionId) +
                " floor=" + current.floorId +
                " name=" + (entry == nullptr ? std::string("?") : entry->floor.floorName) +
                " matches=" + std::to_string(classification.winnerMatches) +
                " runnerUp=" + std::to_string(classification.runnerUpMatches) +
                " heightDirection=" + std::to_string(current.heightDirection) +
                " heightRank=" + std::to_string(current.heightRank) +
                " equivalent=[" + JoinFloors(equivalent) + "]");
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
            decisiveStreak = 0;
            closeStreak = 0;
            sharedGroundFrames = 0;
            sharedGround = false;
            sharedGroundLogged = false;
            outsideFootprintSince = std::chrono::steady_clock::time_point{};
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
            " adopted=" + std::to_string(adopted) +
            " winnerContained=" + std::to_string(winnerContained) +
            " decisiveStreak=" + std::to_string(decisiveStreak) +
            " closeStreak=" + std::to_string(closeStreak) +
            " equivalent=[" + JoinFloors(current.equivalentFloorIds) + "]" +
            " winner=" + std::to_string(classification.winnerMatches) +
            " runnerUp=" + std::to_string(classification.runnerUpMatches) +
            " votes=[" + VoteSummary(classification, candidates) + "]");
    }
}

Scope ScopeHint() {
    std::lock_guard lock(stateMutex);
    return scope;
}

void ObservePosition(int sceneId, double mapX, double mapY) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(stateMutex);
    if (entries.empty()) return;
    // Only the scene the state belongs to can judge it: another scene's coordinates say nothing,
    // and ObserveMinimap ends a state whose scene changed.
    if (!current.active || sceneId == 0 || sceneId != current.sceneId) {
        outsideFootprintSince = std::chrono::steady_clock::time_point{};
        return;
    }
    const auto active = std::find_if(entries.begin(), entries.end(), [&](const Entry& entry) {
        return entry.floor.floorId == current.floorId && entry.sceneId == current.sceneId;
    });
    if (active == entries.end()) {
        outsideFootprintSince = std::chrono::steady_clock::time_point{};
        return;
    }
    if (LayeredFloors::Contains(active->floor, active->transform, mapX, mapY)) {
        outsideFootprintSince = std::chrono::steady_clock::time_point{};
        return;
    }
    if (outsideFootprintSince.time_since_epoch().count() == 0) { outsideFootprintSince = now; return; }
    if (now - outsideFootprintSince < kLeftFootprintFor) return;
    const auto previous = current.floorId;
    current = Snapshot{};
    pendingFloorId.clear();
    pendingCount = 0;
    unknownCount = 0;
    decisiveStreak = 0;
    closeStreak = 0;
    sharedGroundFrames = 0;
    sharedGround = false;
    sharedGroundLogged = false;
    outsideFootprintSince = std::chrono::steady_clock::time_point{};
    ++current.revision;
    Diagnostics::Record("layered-floor-change", "scene=" + std::to_string(sceneId) +
        " floor=cleared previous=" + previous + " reason=left-footprint map=" +
        std::to_string(mapX) + "," + std::to_string(mapY));
}

Snapshot Read() {
    std::lock_guard lock(stateMutex);
    return current;
}

namespace {

// A surface marker - one with no floor, or the entrance marker that stands on the surface above a
// layered map - while a floor is known. It normally hides, but on ground the layer copied from the
// surface it belongs there as much as the layer's own markers do, and the two are drawn together.
MarkerRole SurfaceRole(const Snapshot& state) {
    return state.sharedGround ? MarkerRole::Normal : MarkerRole::Hidden;
}

} // namespace

MarkerRole RoleFor(const ItemDatas& item) {
    const auto state = Read();    if (!state.active) return MarkerRole::Normal;
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
    if (mapId.empty() || floorId.empty()) return SurfaceRole(state); // plain surface collectible
    const int level = LayeredFloors::FloorLevel(floorId);
    // An entrance marker ("-1000000/1") and a floor-less point ("0") sit on the surface.
    if (level == 0 || level <= -1000000) return SurfaceRole(state);
    if (floorId == state.floorId) return MarkerRole::Current;
    // A floor the imagery cannot separate from the current one is not "above" or "below": it is
    // another candidate for where the player is standing, so it draws as the current floor.
    if (std::find(state.equivalentFloorIds.begin(), state.equivalentFloorIds.end(), floorId) !=
        state.equivalentFloorIds.end()) return MarkerRole::Current;
    if (LayeredFloors::FloorLayerId(floorId) != state.layerId) return MarkerRole::Hidden; // another layered map
    // Which way the level runs is a property of the layered map, not of the code: 叩天关's
    // 上层(-1) sits above 下层(-3), while 下层金库's 1楼(-1) sits below 4楼(-4). Assuming the
    // first convention marked the floors above a player standing on 1楼 as below them.
    //
    // The rank, where the floor's name states one, is preferred to the level: a level does not
    // always follow the height (黯原's 虚妄摇篮 is 二层 at the bottom, 入口 in the middle, 一层 on
    // top), and a single direction can never express that.
    const int markerRank = [&] {
        std::lock_guard lock(stateMutex);
        for (const auto& entry : entries) {
            if (entry.floor.floorId == floorId) return entry.floor.heightRank;
        }
        return 0;
    }();
    const int comparison = LayeredFloors::HeightComparison(state.heightRank, state.level, markerRank, level,
        state.heightDirection);
    return comparison < 0 ? MarkerRole::Below : MarkerRole::Above;
}

void Reset() {
    std::lock_guard lock(stateMutex);
    entries.clear();
    current = Snapshot{};
    scope = Scope{};
    pendingFloorId.clear();
    pendingCount = 0;
    unknownCount = 0;
    lastClassifyAt = std::chrono::steady_clock::time_point{};
    lastReportAt = std::chrono::steady_clock::time_point{};
    sharedGroundFrames = 0;
    sharedGround = false;
    sharedGroundLogged = false;
    outsideFootprintSince = std::chrono::steady_clock::time_point{};
}

void SetForTest(const Snapshot& snapshot) {
    std::lock_guard lock(stateMutex);
    current = snapshot;
    outsideFootprintSince = std::chrono::steady_clock::time_point{};
}

void SetEntriesForTest(int sceneId, std::vector<LayeredFloors::FloorEntry> floors,
    LayeredFloors::Transform transform) {
    std::vector<Entry> loaded;
    loaded.reserve(floors.size());
    for (auto& floor : floors) {
        Entry entry;
        entry.sceneId = sceneId;
        entry.kuroStateId = sceneId;   // one scene in these tests, so the two spaces coincide
        entry.regionId = "test";
        entry.floor = std::move(floor);
        entry.transform = transform;
        loaded.push_back(std::move(entry));
    }
    std::lock_guard lock(stateMutex);
    entries = std::move(loaded);
    outsideFootprintSince = std::chrono::steady_clock::time_point{};
}

} // namespace LayeredMap






