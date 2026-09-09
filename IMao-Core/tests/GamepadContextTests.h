#pragma once
#include "Runtime/GamepadContext.h"
#include <string>

inline void TestGamepadContext(void (*check)(bool, const std::string&)) {
    using namespace std::chrono_literals;
    using Clock = GamepadContextSnapshot::Clock;
    const auto start = Clock::time_point{10s};
    const auto markers = [] {
        ItemMarkerFrame frame; frame.profileId = "local"; frame.sceneName = "World"; frame.radius = 100;
        ItemDatas farMarker; farMarker.itemId = "far"; farMarker.nameId = "chest"; farMarker.layer.stateId = 1; farMarker.itemMapROC = {20, 0};
        farMarker.screenCoordiante = {1, 0};
        ItemDatas nearMarker = farMarker; nearMarker.itemId = "near"; nearMarker.itemMapROC = {3, 4}; nearMarker.screenCoordiante = {80, 0};
        ItemDatas completed = nearMarker; completed.itemId = "completed"; completed.isSaved = true;
        ItemDatas outside = nearMarker; outside.itemId = "outside"; outside.itemMapROC = {121, 0};
        frame.markers = {farMarker, nearMarker, completed, outside}; return frame;
    }();
    GamepadContextSnapshot model;
    model.Begin(1, 123, 456);
    model.ObserveUi(1, "local", false, false, true, true, start, 500ms, start);
    model.ObserveMinimap(1, "local", markers, {0, 0}, start, start + 500ms, start);
    const auto gameplay = model.Read("local", start);
    check(gameplay.running && gameplay.observable && gameplay.gameplay && !gameplay.bigMap,
        "current gameplay is explicit and does not authorize gamepad map commands");
    model.ObserveUi(1, "local", false, true, false, true, start + 100ms, 500ms, start + 100ms);
    check(!model.Read("local", start + 100ms).bigMap, "opening candidate is not a stable big map");
    model.ObserveMinimap(1, "local", markers, {20, 0}, start + 150ms, start + 650ms, start + 150ms);
    model.ObserveUi(1, "local", true, true, false, true, start + 200ms, 500ms, start + 200ms);
    const auto opened = model.Read("local", start + 200ms);
    check(opened.bigMap && opened.nearbyAvailable && opened.candidates.size() == 2 &&
        opened.candidates[0].item.itemId == "near" && opened.candidates[0].distance == 5.0,
        "opening freezes filtered candidates ordered by real map distance, not screen or later positions");
    for (int milliseconds = 400; milliseconds <= 2000; milliseconds += 200) {
        const auto observed = start + std::chrono::milliseconds(milliseconds);
        model.ObserveUi(1, "local", true, true, false, true, observed, 500ms, observed);
    }
    const auto ongoing = model.Read("local", start + 2s);
    check(ongoing.nearbyAvailable && ongoing.generation == opened.generation,
        "frozen opening location remains usable during its continuously observed map session");
    model.ObserveMapScene(1, "local", "Tethys", start + 2s);
    const auto otherScene = model.Read("local", start + 2s);
    check(!otherScene.nearbyAvailable && otherScene.candidates.empty() && otherScene.generation != opened.generation,
        "browsing another map scene invalidates old player candidates");
    model.ObserveUi(1, "local", true, false, false, true, start + 2100ms, 500ms, start + 2100ms);
    const auto covered = model.Read("local", start + 2100ms);
    check(!covered.observable && !covered.bigMap && !covered.gameplay && covered.generation != otherScene.generation,
        "missing visual evidence immediately revokes a debounced big map");
    model.ObserveUi(1, "local", true, true, false, true, start + 2200ms, 500ms, start + 2200ms);
    check(!model.Read("local", start + 2200ms).nearbyAvailable,
        "a new map session cannot reuse the previous opening snapshot");
    const auto stalled = model.Read("local", start + 2800ms);
    check(!stalled.observable && !stalled.bigMap, "stalled capture expires context even without new detector callbacks");

    model.Begin(2, 222, 456);
    model.ObserveUi(2, "local", false, false, true, true, start, 500ms, start);
    model.ObserveMinimap(2, "local", markers, {0, 0}, start, start + 150ms, start);
    model.ObserveUi(2, "local", true, true, false, true, start + 200ms, 500ms, start + 200ms);
    check(!model.Read("local", start + 200ms).nearbyAvailable,
        "opening rejects a localization frame whose own freshness deadline already elapsed");
    model.End(1);
    check(model.Read("local", start + 200ms).running, "an old App session cannot stop the new session context");
    const auto beforeProfile = model.Read("local", start + 200ms);
    const auto afterProfile = model.Read("account-b", start + 200ms);
    check(afterProfile.generation != beforeProfile.generation && !afterProfile.bigMap && afterProfile.sceneName.empty(),
        "profile changes revoke pending map and target identities");
    model.End(2);
    check(!model.Read("account-b", start + 200ms).running, "stopping the active App revokes gamepad context");

    model.Begin(3, 333, 456);
    model.ObserveUi(3, "local", false, false, true, true, start, 2s, start);
    model.ObserveMinimap(3, "local", markers, {0, 0}, start, start + 2s, start);
    model.ObserveUi(3, "local", true, true, false, true, start + 1100ms, 500ms, start + 1100ms);
    check(!model.Read("local", start + 1100ms).nearbyAvailable,
        "opening rejects player locations over one second old even with a long frame lifetime");

    model.Begin(4, 444, 456);
    model.ObserveUi(4, "local", false, false, true, true, start, 500ms, start);
    model.ObserveMinimap(4, "local", markers, {0, 0}, start, start + 500ms, start);
    model.ObserveUi(4, "local", true, true, false, true, start + 100ms, 500ms, start + 100ms);
    const auto beforeGap = model.Read("local", start + 100ms);
    model.ObserveUi(4, "local", true, true, false, true, start + 800ms, 500ms, start + 800ms);
    const auto resumed = model.Read("local", start + 800ms);
    check(resumed.generation != beforeGap.generation && !resumed.nearbyAvailable,
        "a resumed capture after a stall cannot silently reuse old targets when no reader observed the gap");

    // World nearby actions read current observations, never the frozen map list.
    model.Begin(5, 555, 456);
    auto liveMarkers = markers; liveMarkers.filterRevision = 7;
    model.ObserveUi(5, "local", false, false, true, true, start, 500ms, start);
    model.ObserveMinimap(5, "local", liveMarkers, {0, 0}, start, start + 500ms, start, 1.0);
    const auto initialNearby = model.ReadNearby("local", start);
    check(initialNearby.available && initialNearby.candidates.size() == 2 && initialNearby.filterRevision == 7,
        "live nearby view carries the actual game, filter and fresh point candidates before opening any map");
    NearbySelection::Session completeSession{initialNearby, NearbySelection::Intent::Complete, 31};
    check(completeSession.Validate(initialNearby, 7, 31, "local", "World", "1:near", start).empty() &&
        !completeSession.Validate(initialNearby, 7, 31, "local", "World", "1:far", start).empty(),
        "completion chooser permits only its explicitly selected original point inside the 15-pixel circle");
    // The registered chooser's display context continues submitting real fresh
    // observations. Reading for five seconds does not freeze or expire the list.
    for (int milliseconds = 200; milliseconds <= 5000; milliseconds += 200) {
        const auto now = start + std::chrono::milliseconds(milliseconds);
        model.ObserveUi(5, "local", false, false, true, true, now, 500ms, now);
        model.ObserveMinimap(5, "local", liveMarkers, {0, 0}, now, now + 500ms, now, 1.0);
    }
    auto latest = model.ReadNearby("local", start + 5s);
    check(completeSession.Validate(latest, 7, 31, "local", "World", "1:near", start + 5s).empty(),
        "an old candidate list remains committable using a newly observed trustworthy player fix after five seconds");
    model.ObserveMinimap(5, "local", liveMarkers, {35, 0}, start + 5s, start + 5500ms, start + 5s, 1.0);
    latest = model.ReadNearby("local", start + 5s);
    check(!completeSession.Validate(latest, 7, 31, "local", "World", "1:near", start + 5s).empty(),
        "moving away rejects the old selected completion point and never substitutes the now-nearer point");
    NearbySelection::Session guideSession{initialNearby, NearbySelection::Intent::Guide, 32};
    check(!guideSession.Validate(latest, 7, 32, "local", "World", "1:far", start + 5s).empty(),
        "nearby guide rejects points outside the same strict 15-pixel completion circle");
    check(!guideSession.Validate(latest, 8, 32, "local", "World", "1:far", start + 5s).empty() &&
        !guideSession.Validate(latest, 7, 33, "local", "World", "1:far", start + 5s).empty() &&
        !guideSession.Validate(latest, 7, 32, "account-b", "World", "1:far", start + 5s).empty() &&
        !guideSession.Validate(latest, 7, 32, "local", "Tethys", "1:far", start + 5s).empty(),
        "nearby submit rejects changed filter, candidate revision, account and scene");
    for (int change = 0; change < 5; ++change) {
        auto invalid = latest;
        if (change == 0) ++invalid.gameHwnd;
        else if (change == 1) ++invalid.gameProcessId;
        else if (change == 2) ++invalid.session;
        else if (change == 3) invalid.available = false;
        else invalid.candidates.clear();
        check(!guideSession.Validate(invalid, 7, 32, "local", "World", "1:far", start + 5s).empty(),
            "nearby submission rejects game/window/session or fresh-candidate availability changes " + std::to_string(change));
    }
    check(!guideSession.Validate(model.ReadNearby("local", start + 5500ms), 7, 32, "local", "World", "1:far", start + 5500ms).empty(),
        "a genuinely stale capture cannot authorize completion or opening a nearby selection");

    ItemMarkerFrame boundaries; boundaries.radius = 120; boundaries.filterRevision = 7;
    auto point = markers.markers.front(); point.itemId = "within"; point.itemMapROC = {14.999, 0};
    boundaries.markers.push_back(point);
    point.itemId = "exact"; point.itemMapROC = {15, 0}; boundaries.markers.push_back(point);
    point.itemId = "edge"; point.itemMapROC = {120, 0}; boundaries.markers.push_back(point);
    point.itemId = "outside"; point.itemMapROC = {120.001, 0}; boundaries.markers.push_back(point);
    boundaries.markers.push_back(boundaries.markers.front());
    auto collected = NearbySelection::Collect(boundaries, {0, 0}, 1.0);
    auto guideGroup = collected;
    NearbySelection::KeepNearestGuideGroup(guideGroup);
    check(guideGroup.size() == 1 && guideGroup.front().item.itemId == "within", "guide excludes exact radius and minimap-wide points");
    auto groupPoint = point; groupPoint.isSaved = false;
    std::vector<NearbySelection::Candidate> group;
    for (int x : {2, 7, 12, -3}) {
        groupPoint.itemId = std::to_string(x); groupPoint.itemMapROC = {double(x), 0};
        group.push_back({groupPoint, double(std::abs(x)), double(std::abs(x))});
    }
    NearbySelection::KeepNearestGuideGroup(group);
    check(group.size() == 3 && std::none_of(group.begin(), group.end(), [](const auto& c) { return c.item.itemId == "12"; }),
        "guide groups only immediate neighbours of nearest, never a transitive chain");
    group.front().item.isSaved = true;
    NearbySelection::KeepNearestGuideGroup(group);
    check(group.size() == 1 && group.front().item.itemId == "-3", "completed nearest is excluded before choosing the next anchor");
    check(collected.size() == 3 && NearbySelection::Includes(collected[0], NearbySelection::Intent::Complete) &&
        !NearbySelection::Includes(collected[1], NearbySelection::Intent::Complete) && collected.back().distance == 120,
        "shared nearby collector uses strict completion <15, inclusive map distance <=120 and deduplicates point identity");
    boundaries.radius = 30;
    check(NearbySelection::Collect(boundaries, {0, 0}, 1.0).size() == 2,
        "nearby guides cannot include points outside the actual minimap even when map distance is within 120");
    boundaries.markers.front().isSaved = true; boundaries.markers.pop_back();
    check(NearbySelection::Collect(boundaries, {0, 0}, 1.0).size() == 1,
        "shared nearby collector excludes current completed points");
    boundaries.radius = 0;
    check(NearbySelection::Collect(boundaries, {0, 0}, 1.0).empty(), "unknown minimap bounds never authorize nearby targets");

    check(NearbySelection::ValidateSingleCompletion(initialNearby, initialNearby, 7, start).empty(),
        "single completion accepts a fresh re-read only while the same canonical point remains uniquely eligible");
    auto overlapping = initialNearby;
    auto entering = overlapping.candidates.back(); entering.item.itemId = "entering"; entering.distance = entering.screenDistance = 4;
    overlapping.candidates.push_back(entering);
    check(NearbySelection::ValidateSingleCompletion(initialNearby, overlapping, 7, start) == "nearby-single-selection-changed",
        "a second point entering the 15-pixel circle before commit rejects direct completion and requires a new choice");
    auto replaced = initialNearby; replaced.candidates.front().item.itemId = "replacement";
    check(!NearbySelection::ValidateSingleCompletion(initialNearby, replaced, 7, start).empty(),
        "a different now-unique point cannot replace the identity chosen by the original shortcut");
    check(!NearbySelection::ValidateSingleCompletion(initialNearby, initialNearby, 7, start + 500ms).empty(),
        "a player observation expiring between initial resolution and the single write is rejected");
    auto replayed = initialNearby;
    int singleWrites = 0;
    if (NearbySelection::ValidateSingleCompletion(initialNearby, replayed, 7, start).empty()) ++singleWrites;
    // The production entry refreshes this durable state even when localization
    // has not published another frame, so the exact same source cannot write twice.
    replayed.candidates.front().item.isSaved = true;
    if (NearbySelection::ValidateSingleCompletion(initialNearby, replayed, 7, start).empty()) ++singleWrites;
    check(singleWrites == 1, "repeated nearby frames cannot re-complete the already persisted single point");
}
