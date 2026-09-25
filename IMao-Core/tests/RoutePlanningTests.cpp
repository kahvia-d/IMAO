#include "Runtime/RoutePlanningModel.h"
#include "Runtime/AutoReplanPolicy.h"
#include "Runtime/RouteGeometry.h"
#include "Runtime/RoutePlanStore.h"
#include "Runtime/PlanningEscapeKey.h"
#include "Runtime/RuntimeHotkeys.h"
#include "Runtime/GuideHotkeyRouting.h"
#include "Runtime/MarkerGuideProtocol.h"
#include <chrono>
#include <iostream>
#include <limits>
#include <random>
#include <set>
#include <thread>

namespace {
int failures = 0;
void Expect(bool condition, const std::string& message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
bool Near(double a, double b) { return std::abs(a - b) <= 1e-7 * std::max({1.0, std::abs(a), std::abs(b)}); }
bool Near(Coordinate a, Coordinate b) { return Near(a.x, b.x) && Near(a.y, b.y); }
template<typename Action> void Rejects(Action action, const std::string& message) {
    bool rejected = false;
    try { action(); } catch (const std::invalid_argument&) { rejected = true; }
    Expect(rejected, message);
}
template<typename Action> void RejectsAny(Action action, const std::string& message) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    Expect(rejected, message);
}
ItemDatas Target(const std::string& id, double x, double y, int state = 1) {
    ItemDatas item;
    item.itemId = id; item.nameId = "chest"; item.itemMapROC = {x, y};
    item.layer = {state, 7, "floor-a", "underground"};
    return item;
}
AutoRoute::Start Origin() { AutoRoute::Start start; start.sceneId = 1; start.valid = true; return start; }
std::vector<ItemDatas> Sample(std::size_t count, unsigned seed) {
    std::mt19937 random(seed);
    std::vector<ItemDatas> result;
    for (std::size_t i = 0; i < count; ++i)
        result.push_back(Target(std::to_string(i), static_cast<int>(random() % 20001) - 10000,
            static_cast<int>(random() % 20001) - 10000));
    return result;
}
std::vector<std::string> Keys(const std::vector<ItemDatas>& targets) {
    std::vector<std::string> result;
    for (const auto& target : targets) result.push_back(AutoRoute::Key(target));
    return result;
}
double MeasuredLength(Coordinate start, const std::vector<ItemDatas>& stops) {
    double sum = 0;
    for (const auto& target : stops) { sum += std::hypot(start.x - target.itemMapROC.x, start.y - target.itemMapROC.y); start = target.itemMapROC; }
    return sum;
}
void SolverTests() {
    const auto start = Origin();
    const auto empty = AutoRoute::Solve(start, {});
    Expect(empty.stops.empty() && empty.planarLength == 0 && !empty.cancelled, "empty target set is a zero-length result");
    const auto single = AutoRoute::Solve(start, {Target("a", 3, 4)});
    Expect(single.stops.size() == 1 && Near(single.planarLength, 5), "single target includes distance from fixed start");
    const auto open = AutoRoute::Solve(start, {Target("b", 101, 0), Target("a", 100, 0)});
    Expect(Near(open.planarLength, 101) && open.stops.front().itemId == "a", "open route has no return-to-start edge");
    auto manual = start; manual.source = "manual"; manual.roc = {200, 0};
    const auto reverse = AutoRoute::Solve(manual, {Target("a", 100, 0), Target("b", 101, 0)});
    Expect(reverse.stops.front().itemId == "b" && Near(reverse.planarLength, 100), "manual start is fixed and changes route order");

    const auto same = AutoRoute::Solve(start, {Target("b", 0, 0), Target("a", 0, 0), Target("a", 0, 0, 2)});
    Expect(Keys(same.stops) == std::vector<std::string>({"1:a", "1:b", "2:a"}) && same.planarLength == 0,
        "co-located distinct identities and state-separated IDs are retained deterministically");
    Expect(same.stops[0].layer.floorId == "floor-a" && same.stops[0].nameId == "chest", "solver preserves point metadata");

    const auto targets = Sample(75, 1091);
    const auto solved = AutoRoute::Solve(start, targets);
    auto expected = Keys(targets), actual = Keys(solved.stops);
    std::sort(expected.begin(), expected.end()); std::sort(actual.begin(), actual.end());
    Expect(actual == expected, "every selected identity occurs exactly once");
    Expect(solved.planarLength < solved.initialLength - 1, "2-opt improves a nontrivial nearest-neighbor route");
    Expect(Near(solved.planarLength, MeasuredLength(start.roc, solved.stops)), "reported length matches independently measured open path");
    auto shuffled = targets;
    std::mt19937 random(23); std::shuffle(shuffled.begin(), shuffled.end(), random);
    const auto again = AutoRoute::Solve(start, shuffled);
    Expect(Keys(again.stops) == Keys(solved.stops) && again.planarLength == solved.planarLength,
        "route output is deterministic independent of selection insertion order");
    for (unsigned seed = 1; seed <= 12; ++seed) {
        const auto result = AutoRoute::Solve(start, Sample(25, seed));
        Expect(result.planarLength <= result.initialLength + 1e-7, "optimization never worsens its nearest-neighbor initial solution");
    }
    auto invalid = start; invalid.valid = false;
    Rejects([&] { AutoRoute::Solve(invalid, {}); }, "missing trusted/manual start is rejected");
    invalid = start; invalid.sceneId = 0;
    Rejects([&] { AutoRoute::Solve(invalid, {}); }, "missing scene is rejected");
    invalid = start; invalid.roc.x = std::numeric_limits<double>::infinity();
    Rejects([&] { AutoRoute::Solve(invalid, {}); }, "nonfinite start is rejected");
    Rejects([&] { AutoRoute::Solve(start, {Target("a", std::numeric_limits<double>::quiet_NaN(), 0)}); }, "nonfinite target rejected");
    Rejects([&] { AutoRoute::Solve(start, {Target("", 1, 2)}); }, "missing identity rejected");
    Rejects([&] { AutoRoute::Solve(start, {Target("a", 1, 2), Target("a", 3, 4)}); }, "duplicate identity rejected as a whole");
    Rejects([&] { AutoRoute::Solve(start, Sample(501, 1)); }, "501 targets rejected without partial selection");
    invalid = start; invalid.roc = {-1e308, 0};
    Rejects([&] { AutoRoute::Solve(invalid, {Target("a", 1e308, 0)}); }, "overflowing coordinate distance rejected");
    const auto immediate = AutoRoute::Solve(start, targets, [] { return true; });
    Expect(immediate.cancelled && immediate.stops.empty(), "immediate cancellation exposes no partial route");
    int polls = 0;
    const auto duringOptimization = AutoRoute::Solve(start, targets, [&] { return ++polls > static_cast<int>(2 * targets.size() + 5); });
    Expect(duringOptimization.cancelled && duringOptimization.stops.empty(), "optimization cancellation exposes no partial route");
}
void GeometryTests() {
    using AutoRoute::ClipRectangle; using AutoRoute::ClipCircle; using AutoRoute::PointInPolygon;
    const std::vector<Coordinate> square{{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    Expect(PointInPolygon({5, 5}, square) && PointInPolygon({0, 5}, square) && PointInPolygon({10, 10}, square), "lasso includes inside, edges and vertices");
    Expect(!PointInPolygon({11, 5}, square) && !PointInPolygon({5, 5}, {{0, 0}, {10, 10}}), "lasso excludes outside and insufficient vertices");
    const std::vector<Coordinate> concave{{0, 0}, {6, 0}, {6, 2}, {2, 2}, {2, 6}, {0, 6}};
    Expect(PointInPolygon({1, 5}, concave) && !PointInPolygon({5, 5}, concave), "lasso follows concave shape instead of its bounding box");
    const std::vector<Coordinate> bowtie{{0, 0}, {10, 10}, {0, 10}, {10, 0}};
    Expect(PointInPolygon({5, 2}, bowtie) && PointInPolygon({5, 8}, bowtie) && !PointInPolygon({1, 5}, bowtie), "self-intersecting lasso uses even-odd lobes");
    auto repeated = square; repeated.push_back(square.front());
    Expect(PointInPolygon({5, 5}, repeated), "explicitly closed lasso tolerates repeated endpoint");
    Expect(!PointInPolygon({0, 0}, {{0, 0}, {1, 1}, {std::numeric_limits<double>::infinity(), 0}}), "invalid lasso rejected");

    const auto crossing = ClipRectangle({-5, 5}, {15, 5}, 0, 0, 10, 10);
    Expect(crossing && Near(crossing->first, {0, 5}) && Near(crossing->second, {10, 5}), "rectangle retains crossing whose endpoints are both outside");
    const auto reverse = ClipRectangle({15, 5}, {-5, 5}, 0, 0, 10, 10);
    Expect(reverse && Near(reverse->first, {10, 5}) && Near(reverse->second, {0, 5}), "clipping preserves direction");
    Expect(!ClipRectangle({-2, -1}, {12, -1}, 0, 0, 10, 10), "parallel outside segment omitted");
    const auto corner = ClipRectangle({-1, 1}, {1, -1}, 0, 0, 10, 10);
    Expect(corner && Near(corner->first, {0, 0}) && Near(corner->second, {0, 0}), "rectangle corner tangent retained");
    Expect(ClipRectangle({5, 5}, {5, 5}, 0, 0, 10, 10).has_value() && !ClipRectangle({15, 5}, {15, 5}, 0, 0, 10, 10), "rectangle handles zero-length segment");
    // Adjacent original edges outside the viewport must not create a chord between their surviving vertices.
    const auto first = ClipRectangle({1, 1}, {20, 1}, 0, 0, 10, 10);
    const auto middle = ClipRectangle({20, 1}, {20, 9}, 0, 0, 10, 10);
    const auto last = ClipRectangle({20, 9}, {1, 9}, 0, 0, 10, 10);
    Expect(first && !middle && last && Near(first->second, {10, 1}) && Near(last->first, {10, 9}), "independent clipping preserves outside excursion gap");

    const auto circle = ClipCircle({-20, 0}, {20, 0}, {0, 0}, 10);
    Expect(circle && Near(circle->first, {-10, 0}) && Near(circle->second, {10, 0}), "circle retains outside-to-outside crossing");
    const auto tangent = ClipCircle({-20, 10}, {20, 10}, {0, 0}, 10);
    Expect(tangent && Near(tangent->first, {0, 10}) && Near(tangent->second, {0, 10}), "circle tangent retained");
    Expect(!ClipCircle({-20, 11}, {20, 11}, {0, 0}, 10), "outside circle segment omitted");
    const auto circleInside = ClipCircle({1, 1}, {2, 2}, {0, 0}, 10);
    Expect(circleInside && Near(circleInside->first, {1, 1}) && Near(circleInside->second, {2, 2}), "inside circle segment unchanged");
    Expect(ClipCircle({2, 2}, {2, 2}, {0, 0}, 10).has_value() && !ClipCircle({20, 20}, {20, 20}, {0, 0}, 10), "circle handles zero-length segment");
    const auto radiusZero = ClipCircle({-1, 0}, {1, 0}, {0, 0}, 0);
    Expect(radiusZero && Near(radiusZero->first, {0, 0}) && Near(radiusZero->second, {0, 0}), "zero radius clips to center");
    Expect(!ClipCircle({0, 0}, {1, 1}, {0, 0}, -1) && !ClipRectangle({0, 0}, {1, 1}, 10, 0, 0, 10), "invalid clip bounds rejected");
}
void StoreTests() {
    namespace fs = std::filesystem;
    using Json = nlohmann::json;
    const auto base = fs::weakly_canonical(fs::temp_directory_path());
    const auto folder = base / ("imao-route-tests-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    if (!fs::create_directory(folder)) throw std::runtime_error("route test directory already exists");
    struct Cleanup {
        fs::path folder, base;
        ~Cleanup() {
            std::error_code error;
            const auto resolved = fs::weakly_canonical(folder, error);
            if (!error && resolved.parent_path() == base && resolved.filename().string().starts_with("imao-route-tests-"))
                fs::remove_all(resolved, error);
        }
    } cleanup{folder, base};
    const auto read = [](const fs::path& path) { std::ifstream input(path, std::ios::binary); return std::string(std::istreambuf_iterator<char>(input), {}); };
    const auto legacyPath = folder / "SavedRoutes" / "legacy.json";
    const std::string legacy = R"({"World":[[[0,0],[1,1]]]})";
    WriteTextAtomically(legacyPath, legacy);
    AutoRoute::RoutePlanStore store(folder / "SavedRoutes" / "Auto");
    AutoRoute::Plan plan;
    plan.id = "route-1"; plan.name = "test route"; plan.profileId = "local"; plan.sceneId = 1; plan.start = Origin();
    plan.start.confirmedUnixMs = 1790000000000; plan.start.generation = 42;
    const int state = Scene::Find(plan.sceneId)->kuroStateId;
    plan.stops = {Target("a", 1, 2, state), Target("b", 4, 5, state)};
    plan.stops.front().isSaved = true;
    plan.skipped.insert(AutoRoute::Key(plan.stops.back()));
    plan.skipHistory.push_back(AutoRoute::Key(plan.stops.back()));
    const auto resolver = [&](int sceneId, const std::string& key) -> std::optional<ItemDatas> {
        if (sceneId != plan.sceneId) return {};
        for (const auto& item : plan.stops) if (AutoRoute::Key(item) == key) return item;
        return {};
    };
    Expect(!store.LoadActive("local", resolver), "no explicit active pointer means no route is guessed");
    store.Save(plan, true);
    const auto restored = store.Load("local", "route-1", resolver);
    Expect(restored.id == plan.id && restored.profileId == plan.profileId && restored.sceneId == plan.sceneId && restored.name == plan.name &&
        Keys(restored.stops) == Keys(plan.stops) && restored.skipped == plan.skipped && restored.skipHistory == plan.skipHistory,
        "store roundtrip preserves plan identity, order, skips and undo history");
    Expect(restored.start.confirmedUnixMs == plan.start.confirmedUnixMs && restored.start.generation == 42 && restored.start.source == "playerSnapshot" &&
        restored.stops[0].layer.floorId == "floor-a" && Near(restored.stops[0].itemMapROC, {1, 2}), "store roundtrip preserves start and target metadata");
    const auto planPath = folder / "SavedRoutes" / "Auto" / "local" / "route-1.json";
    const auto document = Json::parse(read(planPath));
    Expect(!document.contains("completed") && !document.at("stops")[0].contains("completed") && !document.at("stops")[0].contains("isSaved") &&
        document.at("stops")[1].at("skipped").get<bool>(), "stored skips are independent of authoritative marker completion");
    auto priorVersion = document; priorVersion.erase("skipHistory");
    WriteTextAtomically(planPath, priorVersion.dump());
    Expect(store.Load("local", "route-1", resolver).skipHistory == plan.skipHistory, "existing v1 skips regain undo history in route order");
    store.Save(plan, false);
    auto invalidHistory = plan; invalidHistory.skipHistory.push_back(plan.skipHistory.front());
    RejectsAny([&] { store.Save(invalidHistory); }, "duplicate skip undo history rejected");
    invalidHistory = plan; invalidHistory.skipHistory.push_back(AutoRoute::Key(plan.stops.front()));
    RejectsAny([&] { store.Save(invalidHistory); }, "history cannot reference an unskipped target");
    const auto newCompletion = [&](int scene, const std::string& key) {
        auto item = resolver(scene, key); if (item) item->isSaved = false; return item;
    };
    Expect(!store.Load("local", "route-1", newCompletion).stops[0].isSaved, "restoration uses current marker data instead of stale completed flag");
    auto second = plan; second.id = "route-2"; second.name = "second route";
    store.Save(second, false);
    Expect(store.LoadActive("local", resolver)->id == "route-1", "ordinary save does not replace explicit active route");
    store.Save(second, true);
    Expect(store.LoadActive("local", resolver)->id == "route-2", "explicit make-active changes active pointer");
    Expect(store.List("local").size() == 2, "listing excludes active pointer");

    const auto before = read(planPath);
    HANDLE locked = CreateFileW(planPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Expect(locked != INVALID_HANDLE_VALUE, "can lock destination for failed atomic-save test");
    if (locked != INVALID_HANDLE_VALUE) {
        auto changed = plan; changed.name = "must not persist";
        RejectsAny([&] { store.Save(changed, true); }, "failed atomic replacement is surfaced");
        CloseHandle(locked);
        Expect(read(planPath) == before && store.LoadActive("local", resolver)->id == "route-2", "failed plan save leaves file and active pointer intact");
    }
    RejectsAny([&] { store.Load("local", "route-1", [](int, const std::string&) -> std::optional<ItemDatas> { return {}; }); }, "unknown target blocks restore");
    RejectsAny([&] { store.Load("local", "route-1", [&](int scene, const std::string& key) {
        auto item = resolver(scene, key); if (item) item->itemId += "-wrong"; return item;
    }); }, "resolver must return the requested stable identity");
    RejectsAny([&] { store.Load("local", "route-1", [&](int scene, const std::string& key) {
        auto item = resolver(scene, key); if (item) item->itemMapROC.x += 1; return item;
    }); }, "coordinate resource change blocks restore");
    WriteTextAtomically(planPath, "{broken");
    RejectsAny([&] { store.Load("local", "route-1", resolver); }, "corrupt route is rejected");
    Expect(store.List("local").size() == 2, "corrupt route remains visible for diagnosis");
    store.Save(plan, false);
    for (const auto* unsafe : {"../escape", "..\\escape", "C:escape", ""}) {
        RejectsAny([&] { store.Load(unsafe, "route-1", resolver); }, "profile traversal is rejected");
        RejectsAny([&] { store.Load("local", unsafe, resolver); }, "route identifier traversal is rejected");
    }
    auto reserved = plan; reserved.id = "Active";
    RejectsAny([&] { store.Save(reserved); }, "case-insensitive active pointer collision is rejected");
    Expect(read(legacyPath) == legacy, "legacy sibling route file remains byte-for-byte unchanged");

    const auto pointerPath = planPath.parent_path() / "active.json";
    const auto secondPath = planPath.parent_path() / "route-2.json";
    const auto firstBytes = read(planPath), secondBytes = read(secondPath);
    auto otherProfile = second; otherProfile.profileId = "other-profile";
    store.Save(otherProfile, true);
    store.ClearActive("local");
    Expect(!store.LoadActive("local", resolver) && read(planPath) == firstBytes && read(secondPath) == secondBytes,
        "clearing active pointer stops restoration without deleting saved routes or their progress");
    store.ClearActive("local");
    Expect(!store.LoadActive("local", resolver) && store.LoadActive("other-profile", resolver)->id == "route-2",
        "clear-active is idempotent and isolated from the other profile");
    store.Save(second, true);
    locked = CreateFileW(pointerPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Expect(locked != INVALID_HANDLE_VALUE, "can lock active pointer for stop/delete failure tests");
    if (locked != INVALID_HANDLE_VALUE) {
        RejectsAny([&] { store.ClearActive("local"); }, "failed pointer clear is surfaced");
        RejectsAny([&] { store.Delete("local", "route-2"); }, "delete reports a failed active-pointer clear");
        CloseHandle(locked);
        Expect(store.LoadActive("local", resolver)->id == "route-2" && read(secondPath) == secondBytes &&
            !fs::exists(fs::path(secondPath.string() + ".deleting")),
            "failed active deletion rolls the renamed route back and preserves its original active pointer");
    }
    WriteTextAtomically(planPath, "{broken route awaiting deletion");
    store.Delete("local", "route-1");
    Expect(!fs::exists(planPath) && store.LoadActive("local", resolver)->id == "route-2" && store.List("local").size() == 1,
        "damaged inactive route can be deleted without parsing it or stopping a different active route");
    RejectsAny([&] { store.Delete("local", "missing-route"); }, "deleting a missing route is rejected");
    for (const auto* unsafe : {"../escape", "..\\escape", "C:escape", ""}) {
        RejectsAny([&] { store.ClearActive(unsafe); }, "clear-active rejects profile traversal");
        RejectsAny([&] { store.Delete(unsafe, "route-2"); }, "delete rejects profile traversal");
        RejectsAny([&] { store.Delete("local", unsafe); }, "delete rejects route traversal");
    }
    Expect(store.LoadActive("local", resolver)->id == "route-2", "rejected deletion attempts preserve the active route");
    locked = CreateFileW(secondPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Expect(locked != INVALID_HANDLE_VALUE, "can lock route file for failed deletion test");
    if (locked != INVALID_HANDLE_VALUE) {
        RejectsAny([&] { store.Delete("local", "route-2"); }, "locked route deletion is rejected before clearing active pointer");
        CloseHandle(locked);
        Expect(read(secondPath) == secondBytes && store.LoadActive("local", resolver)->id == "route-2",
            "failed route deletion changes neither saved bytes nor active pointer");
    }
    store.Delete("local", "ROUTE-2");
    Expect(!fs::exists(secondPath) && !store.LoadActive("local", resolver) && store.List("local").empty(),
        "deleting active route removes it from disk and prevents automatic restoration");
    Expect(store.LoadActive("other-profile", resolver)->id == "route-2" && read(legacyPath) == legacy,
        "same-ID route in another profile and all legacy route files survive deletion");
    for (const auto& entry : fs::directory_iterator(planPath.parent_path()))
        Expect(entry.path().filename().string().find(".tmp-") == std::string::npos, "atomic save leaves no abandoned temporary files");
}
void EscapeOwnershipTests() {
    using AutoRoute::EscapeAction;
    AutoRoute::PlanningEscapeKey key;
    Expect(key.Handle(false, true, true, false) == EscapeAction::PassThrough, "unowned Escape release passes through");
    Expect(key.Handle(true, true, true, true) == EscapeAction::CancelGesture, "first Escape cancels only the current gesture");
    Expect(key.Handle(true, true, true, false) == EscapeAction::Consume, "held Escape cannot also return to pan after cancelling gesture");
    Expect(key.Handle(false, true, true, false) == EscapeAction::Consume, "gesture-cancelling Escape owns its matching release");
    Expect(key.Handle(true, true, true, false) == EscapeAction::ReturnToPan, "a separate Escape press returns from selection to pan");
    Expect(key.Handle(true, false, false, false) == EscapeAction::Consume, "owned Escape repeats stay consumed after mode and focus loss");
    Expect(key.Handle(false, false, false, false) == EscapeAction::Consume, "owned Escape release stays consumed after mode and focus loss");
    Expect(key.Handle(true, false, true, false) == EscapeAction::PassThrough, "normal gameplay Escape belongs to the game");
    Expect(key.Handle(true, true, true, true) == EscapeAction::PassThrough, "entering planning does not steal an already-held game Escape");
    Expect(key.Handle(false, true, true, true) == EscapeAction::PassThrough, "unowned gameplay Escape keeps its release");
    Expect(key.Handle(true, true, false, true) == EscapeAction::PassThrough, "Escape beginning outside the focused game is not intercepted");
    Expect(key.Handle(true, true, true, true) == EscapeAction::PassThrough, "regaining game focus does not steal Escape repeats");
    Expect(key.Handle(false, true, true, true) == EscapeAction::PassThrough, "outside-app Escape release remains unowned");
    Expect(key.Handle(true, true, true, true) == EscapeAction::CancelGesture, "next fresh Escape works after an unowned key sequence");
    key.Reset();
    Expect(key.Handle(false, false, false, false) == EscapeAction::PassThrough, "hook reset discards previous lifecycle ownership");
}
void DrawingVisibilityTests() {
    RouteDatas legacy("legacy", 1, {{0, 0}, {10, 10}});
    RouteDatas active("automatic", 1, {{0, 0}, {10, 10}});
    active.automatic = true; active.profileId = "local"; active.routePlanId = "route-a";
    RouteDatas preview = active; preview.preview = true; preview.routePlanId = "preview-a";
    AutoRoute::DrawVisibility visible{"local", "route-a", "preview-a", true};
    Expect(visible.Allows(active) && visible.Allows(active, true), "current automatic route may draw on both maps while navigating");
    Expect(visible.Allows(preview) && !visible.Allows(preview, true), "preview draws only on the large map");
    visible.navigating = false;
    Expect(visible.Allows(active) && !visible.Allows(active, true), "pause retains the large-map route but immediately hides cached minimap guidance");
    const AutoRoute::DrawVisibility stopped{"local", "", "", false};
    Expect(!stopped.Allows(active) && !stopped.Allows(active, true) && !stopped.Allows(preview),
        "stop rejects previously captured automatic segments even when localization has stopped publishing frames");
    visible.activeId = "route-b"; visible.previewId = "preview-b"; visible.navigating = true;
    Expect(!visible.Allows(active) && !visible.Allows(active, true) && !visible.Allows(preview),
        "replacing active route or preview rejects segments retained from their previous IDs");
    const AutoRoute::DrawVisibility otherProfile{"other", "route-a", "preview-a", true};
    Expect(!otherProfile.Allows(active) && !otherProfile.Allows(preview), "matching route IDs from a different profile cannot reuse cached geometry");
    active.routePlanId.clear();
    Expect(!stopped.Allows(active) && !visible.Allows(active), "automatic segments without stable plan identity are rejected");
    Expect(stopped.Allows(legacy) && stopped.Allows(legacy, true) && otherProfile.Allows(legacy),
        "legacy manually drawn routes remain governed by their existing rendering rules");
}
void HotkeyPressOwnershipTests() {
    using AutoRoute::EscapeAction;
    RuntimeHotkeyPressOwnership::Reset();
    RuntimeHotkeys::Apply({});
    std::array<AutoRoute::PlanningEscapeKey, 256> keys;
    const auto hook = [&](int vk, bool down, bool guideOwns, bool focused = true) {
        const bool firstDown = down && !keys[vk].IsPressed();
        const auto action = keys[vk].Handle(down, guideOwns, focused, false);
        if (down) RuntimeHotkeyPressOwnership::RecordKeyDown(vk, firstDown, action != EscapeAction::PassThrough);
        return action;
    };
    bool sampledDown = false;
    const auto gamePoll = [&](int vk, bool asyncDown) {
        const bool completed = asyncDown && !sampledDown && !RuntimeHotkeyPressOwnership::BlocksPolling(vk);
        sampledDown = asyncDown; // Keep the physical edge, even for suppressed presses.
        return completed;
    };

    Expect(hook(90, true, true) == EscapeAction::ReturnToPan, "guide owns the first completion-key down");
    // No game poll ran before the completion saved and the guide hid. The first
    // game-focused poll must not reinterpret this same physical press.
    Expect(!gamePoll(90, true), "guide completion cannot also complete a nearby point when hiding beats the first game poll");
    Expect(hook(90, true, false) == EscapeAction::Consume && RuntimeHotkeyPressOwnership::BlocksPolling(90),
        "focus returning to the game cannot transfer a held guide press");
    Expect(hook(90, false, false) == EscapeAction::Consume && RuntimeHotkeyPressOwnership::BlocksPolling(90),
        "owned key-up preserves the polling fence until a new physical press");
    gamePoll(90, false);
    Expect(!gamePoll(90, true), "stale asynchronous down state after owned key-up cannot create a completion");
    gamePoll(90, false);
    Expect(hook(90, true, false) == EscapeAction::PassThrough && gamePoll(90, true),
        "the next fresh unowned game press can complete exactly once");
    Expect(!gamePoll(90, true), "subsequent polls of the same ordinary game press do not repeat completion");
    hook(90, false, false);
    gamePoll(90, false);

    hook(90, true, true);
    RuntimeHotkeys::Apply({65, 81, 119});
    hook(90, true, false);
    Expect(RuntimeHotkeyPressOwnership::BlocksPolling(90), "rebinding while a guide key is held cannot clear its ownership");
    Expect(hook(65, true, false) == EscapeAction::PassThrough && !RuntimeHotkeyPressOwnership::BlocksPolling(65) &&
        RuntimeHotkeyPressOwnership::BlocksPolling(90), "fresh input on another VK does not release the original key fence");
    RuntimeHotkeys::Apply({});
    Expect(RuntimeHotkeyPressOwnership::BlocksPolling(RuntimeHotkeys::Snapshot().nearestCompletionKey),
        "restoring a binding retains the physical key's existing ownership");
    hook(90, false, false);
    hook(90, true, true);
    Expect(RuntimeHotkeyPressOwnership::BlocksPolling(90), "a second guide-owned press remains fenced");
    hook(90, false, false);
    hook(90, true, false, false);
    Expect(!RuntimeHotkeyPressOwnership::BlocksPolling(90), "a fresh unowned press establishes a new owner even outside the game");
    Expect(hook(90, true, true) == EscapeAction::PassThrough && !RuntimeHotkeyPressOwnership::BlocksPolling(90),
        "an already-held unowned key cannot be claimed by a newly focused guide");
    RuntimeHotkeyPressOwnership::RecordKeyDown(90, true, true);
    RuntimeHotkeyPressOwnership::RecordKeyDown(90, false, false);
    Expect(RuntimeHotkeyPressOwnership::BlocksPolling(90), "a repeat cannot accidentally clear the polling fence");
    for (const int invalid : {-1, 0, 256}) {
        RuntimeHotkeyPressOwnership::RecordKeyDown(invalid, true, true);
        Expect(!RuntimeHotkeyPressOwnership::BlocksPolling(invalid), "disabled and out-of-range VKs cannot own a polling press");
    }
    RuntimeHotkeyPressOwnership::Reset();
    Expect(!RuntimeHotkeyPressOwnership::BlocksPolling(90), "lifecycle reset clears old hook ownership");
}

// 攻略窗口可见时，Z（完成当前点位）与 G（长按跳过）必须归攻略窗口所有——**不要求攻略窗口
// 是前台窗口**。实机反馈：只有先用鼠标点一下攻略窗口，Z 与 G 才生效；玩家在游戏里按键时
// 窗口拿不到键盘焦点，所以"谁是前台窗口"这条判定必须放在这里，而不是要求窗口自己在前台。
// 开关攻略（F8）是例外：它必须在还没有攻略窗口时也能用。
//
// 用例必须按钩子**实际传的那一组参数**来写。2026-09-25 就是在这里漏了一步：钩子里在函数外面
// 多写了一个 `&& guideIdentity`，于是"还没有攻略窗口"的 F8 打不开任何攻略，而当时的用例只测了
// 函数本身、全绿通过。现在钩子只调用这一个函数，下面每条都对着它的一种真实输入。
void GuideHotkeyRoutingTests() {
    using AutoRoute::GuideHotkeyKind;
    const RuntimeHotkeyBindings bindings{};
    Expect(AutoRoute::ClassifyGuideHotkey(bindings, 90) == GuideHotkeyKind::CompleteShownPoint &&
        AutoRoute::ClassifyGuideHotkey(bindings, 71) == GuideHotkeyKind::Skip &&
        AutoRoute::ClassifyGuideHotkey(bindings, 119) == GuideHotkeyKind::ToggleGuide &&
        AutoRoute::ClassifyGuideHotkey(bindings, 33) == GuideHotkeyKind::PageBack &&
        AutoRoute::ClassifyGuideHotkey(bindings, 34) == GuideHotkeyKind::PageForward &&
        AutoRoute::ClassifyGuideHotkey(bindings, 65) == GuideHotkeyKind::None,
        "each configured guide key maps to its own action and no other key is claimed");

    auto disabled = bindings;
    disabled.guideSkipKey = 0;
    Expect(AutoRoute::ClassifyGuideHotkey(disabled, 71) == GuideHotkeyKind::None,
        "a disabled binding stops claiming its key");

    // 开/关攻略：**还没有攻略窗口**（游戏中按 F8 打开它）也必须归我们，否则这个键什么都打不开。
    // 这是 2026-09-25 实机回归的那一条：可见性/身份都不能参与 ToggleGuide 的判定。
    Expect(AutoRoute::GuideHotkeyOwned(GuideHotkeyKind::ToggleGuide, /*modifiers*/ false,
        /*guideVisible*/ false, /*guideIdentity*/ false, /*guideFocused*/ false, /*gameFocused*/ true),
        "the guide key opens the guide when no guide window and no identity exist yet");
    Expect(AutoRoute::GuideHotkeyOwned(GuideHotkeyKind::ToggleGuide, false, true, true, false, true),
        "the guide key also closes a guide the game is not focusing");
    Expect(!AutoRoute::GuideHotkeyOwned(GuideHotkeyKind::ToggleGuide, false, true, true, false, false),
        "the guide key stays with the game while a third application is in front");
    Expect(!AutoRoute::GuideHotkeyOwned(GuideHotkeyKind::ToggleGuide, /*modifiers*/ true, false, false, false, true),
        "a modified guide key is never ours");

    // 这一条就是 Z/G 的实机 bug：攻略窗口可见、玩家在游戏里按键（前台是游戏，不是攻略窗口）。
    for (const auto kind : {GuideHotkeyKind::CompleteShownPoint, GuideHotkeyKind::Skip})
        Expect(AutoRoute::GuideHotkeyOwned(kind, false, /*guideVisible*/ true, /*guideIdentity*/ true,
            /*guideFocused*/ false, /*gameFocused*/ true),
            "a visible guide owns completion and skip while the game - not the guide - holds the foreground");
    Expect(AutoRoute::GuideHotkeyOwned(GuideHotkeyKind::Skip, false, true, true, /*guideFocused*/ true, false),
        "a focused guide still owns its keys");
    Expect(!AutoRoute::GuideHotkeyOwned(GuideHotkeyKind::CompleteShownPoint, false, true, true, false, false) &&
        !AutoRoute::GuideHotkeyOwned(GuideHotkeyKind::Skip, false, true, true, false, false),
        "another application in front means these keys are not ours");
    Expect(!AutoRoute::GuideHotkeyOwned(GuideHotkeyKind::CompleteShownPoint, false, false, false, false, true) &&
        !AutoRoute::GuideHotkeyOwned(GuideHotkeyKind::Skip, false, false, false, false, true),
        "completion and skip need a visible guide; with none they stay with the game");
    Expect(!AutoRoute::GuideHotkeyOwned(GuideHotkeyKind::CompleteShownPoint, false, true, /*guideIdentity*/ false, false, true) &&
        !AutoRoute::GuideHotkeyOwned(GuideHotkeyKind::Skip, false, true, false, false, true),
        "an overlap chooser without a selected point keeps the key with the game");
    Expect(!AutoRoute::GuideHotkeyOwned(GuideHotkeyKind::Skip, true, true, true, true, true) &&
        !AutoRoute::GuideHotkeyOwned(GuideHotkeyKind::CompleteShownPoint, true, true, true, true, true),
        "a modified completion or skip key is never ours");
    Expect(!AutoRoute::GuideHotkeyOwned(GuideHotkeyKind::None, false, true, true, true, true) &&
        !AutoRoute::GuideHotkeyOwned(GuideHotkeyKind::PageBack, false, true, true, true, true),
        "unrelated keys and paging (decided by PageRequest) are never claimed here");

    // 跳过键的抬起只有"那个可见且有身份的窗口"才需要转交，用来停掉窗口那侧的 600 毫秒计时。
    Expect(AutoRoute::GuideSkipReleaseDelivered(/*guideVisible*/ true, /*guideIdentity*/ true) &&
        !AutoRoute::GuideSkipReleaseDelivered(false, false) && !AutoRoute::GuideSkipReleaseDelivered(true, false),
        "a skip key-up is forwarded only while the identified guide is still visible");

    // 手柄的攻略快捷键是开关：同一档案的攻略窗口已经开着时，这一下是关掉它。
    Expect(AutoRoute::GuideShortcutClosesVisibleGuide(/*guideIntent*/ true, /*gamepad*/ true, /*guideVisible*/ true, /*sameProfile*/ true),
        "the gamepad guide shortcut closes a guide window that is already open");
    Expect(!AutoRoute::GuideShortcutClosesVisibleGuide(true, true, true, /*sameProfile*/ false),
        "a guide window belonging to another profile is not ours to close");
    Expect(!AutoRoute::GuideShortcutClosesVisibleGuide(true, true, /*guideVisible*/ false, true),
        "with no guide window the same shortcut opens one instead");
    Expect(!AutoRoute::GuideShortcutClosesVisibleGuide(/*guideIntent*/ false, true, true, true),
        "the completion chord never closes a guide");
    Expect(!AutoRoute::GuideShortcutClosesVisibleGuide(true, /*gamepad*/ false, true, true),
        "the keyboard guide key decides open/close in the managed layer, not here");

    // 世界手柄和弦（LB+B 完成、LB+X 攻略）在"游戏前台"或"我们自己的攻略窗口前台"时都可用；
    // 别的程序在前台时两个条件都是 false，和弦照旧不生效。
    Expect(AutoRoute::WorldChordAllowed(/*gameFocused*/ true, /*ourGuideFocused*/ false) &&
        AutoRoute::WorldChordAllowed(false, /*ourGuideFocused*/ true) &&
        AutoRoute::WorldChordAllowed(true, true),
        "world chords work with the game in front or with our own guide window in front");
    Expect(!AutoRoute::WorldChordAllowed(false, false),
        "another application in front keeps the world chords unavailable");

    // 攻略图片的固定按键：Enter（放大/退出大图）与 ESC（退出大图）。它们不进可配置绑定表，
    // 所以分类单独一条；每条归属断言都对着钩子真实传的那组参数。
    using AutoRoute::GuidePictureKeyKind;
    Expect(AutoRoute::ClassifyGuidePictureKey(AutoRoute::GuidePictureEnterKey) == GuidePictureKeyKind::Enter &&
        AutoRoute::ClassifyGuidePictureKey(AutoRoute::GuidePictureEscapeKey) == GuidePictureKeyKind::Escape &&
        AutoRoute::ClassifyGuidePictureKey(90) == GuidePictureKeyKind::None &&
        AutoRoute::ClassifyGuidePictureKey(71) == GuidePictureKeyKind::None &&
        AutoRoute::ClassifyGuidePictureKey(0) == GuidePictureKeyKind::None,
        "Enter and Escape are classified as picture keys and no configurable guide key is");
    // Enter：攻略可见 + 游戏在前台（F8 打开的攻略窗口在生产里常常拿不到前台）。
    Expect(AutoRoute::GuidePictureKeyOwned(GuidePictureKeyKind::Enter, /*guideVisible*/ true, /*pictureVisible*/ false,
        /*gameFocused*/ true, /*guideFocused*/ false),
        "Enter enlarges the picture while the game - not the guide - holds the foreground");
    Expect(AutoRoute::GuidePictureKeyOwned(GuidePictureKeyKind::Enter, true, false, false, /*guideFocused*/ true),
        "a focused guide window owns Enter as well");
    Expect(!AutoRoute::GuidePictureKeyOwned(GuidePictureKeyKind::Enter, /*guideVisible*/ false, false, true, false),
        "with no guide window Enter stays with the game");
    Expect(!AutoRoute::GuidePictureKeyOwned(GuidePictureKeyKind::Enter, true, false, false, false),
        "another application in front keeps Enter with that application");
    // ESC：只有大图真的开着才归攻略——否则会把大地图的"取消手势/回到平移"吃掉。
    Expect(AutoRoute::GuidePictureKeyOwned(GuidePictureKeyKind::Escape, /*guideVisible*/ true, /*pictureVisible*/ true,
        /*gameFocused*/ true, /*guideFocused*/ false),
        "Esc closes the enlarged picture while the game holds the foreground");
    Expect(!AutoRoute::GuidePictureKeyOwned(GuidePictureKeyKind::Escape, true, /*pictureVisible*/ false, true, true),
        "Esc without an open enlarged picture stays with the map gestures");
    Expect(!AutoRoute::GuidePictureKeyOwned(GuidePictureKeyKind::Escape, /*guideVisible*/ false, true, true, false),
        "Esc with no guide window at all is never ours");
    Expect(!AutoRoute::GuidePictureKeyOwned(GuidePictureKeyKind::None, true, true, true, true),
        "an unclassified key is never claimed by the picture routing");
}

void HotkeyConfigurationTests() {
    using Json = nlohmann::json;
    const auto same = [](RuntimeHotkeyBindings a, RuntimeHotkeyBindings b) {
        return a.nearestCompletionKey == b.nearestCompletionKey && a.manualRouteKey == b.manualRouteKey &&
            a.currentTargetGuideKey == b.currentTargetGuideKey && a.guidePreviousImageKey == b.guidePreviousImageKey &&
            a.guideNextImageKey == b.guideNextImageKey;
    };
    RuntimeHotkeys::Apply({});
    const auto defaults = RuntimeHotkeys::Snapshot();
    Expect(same(defaults, {90, 81, 119, 33, 34}), "default bindings are Z, Q, F8 and PageUp/PageDown guide pagination");
    Expect(RuntimeHotkeys::Label(defaults.currentTargetGuideKey) == "F8" && RuntimeHotkeys::Label(90) == "Z" &&
        RuntimeHotkeys::Label(33) == "PageUp" && RuntimeHotkeys::Label(34) == "PageDown", "hotkey display labels match all five keys");
    RuntimeHotkeys::Apply({0, 0, 0, 0, 0});
    Expect(same(RuntimeHotkeys::Snapshot(), {0, 0, 0, 0, 0}), "all five bindings may be disabled without a duplicate-key conflict");
    const auto partialDisabled = RuntimeHotkeys::ValidateConfiguration({{"nearestCompletionKey", 65}});
    RuntimeHotkeys::Apply(partialDisabled);
    Expect(same(RuntimeHotkeys::Snapshot(), {65, 0, 0, 0, 0}), "partial hotkey update preserves unspecified disabled bindings");
    RuntimeHotkeys::Apply(defaults);
    const auto partial = RuntimeHotkeys::ValidateConfiguration({{"currentTargetGuideKey", 112}});
    Expect(same(partial, {90, 81, 112}) && same(RuntimeHotkeys::Snapshot(), defaults), "validation preserves unspecified defaults and never publishes before Apply");
    for (const int reserved : {27, 77, 121, 16, 17, 18, -1, 256}) {
        RejectsAny([&] { RuntimeHotkeys::Apply({reserved, 81, 119}); }, "reserved, modifier or out-of-range key cannot be configured");
        Expect(same(RuntimeHotkeys::Snapshot(), defaults), "rejected key leaves published bindings unchanged");
    }
    const std::array<int RuntimeHotkeyBindings::*, 5> fields{&RuntimeHotkeyBindings::nearestCompletionKey,
        &RuntimeHotkeyBindings::manualRouteKey, &RuntimeHotkeyBindings::currentTargetGuideKey,
        &RuntimeHotkeyBindings::guidePreviousImageKey, &RuntimeHotkeyBindings::guideNextImageKey};
    for (std::size_t i = 0; i < fields.size(); ++i) for (std::size_t j = i + 1; j < fields.size(); ++j) {
        auto duplicate = defaults; duplicate.*fields[j] = duplicate.*fields[i];
        RejectsAny([&] { RuntimeHotkeys::Apply(duplicate); }, "every pair of nonzero bindings rejects a duplicate key");
        Expect(same(RuntimeHotkeys::Snapshot(), defaults), "duplicate binding rejection leaves all five keys unchanged");
    }
    for (const auto* field : {"currentTargetGuideKey", "guidePreviousImageKey", "guideNextImageKey"})
    for (const Json malformed : {Json(1.5), Json(true), Json("F8"), Json(nullptr), Json(-1), Json(256)}) {
        Json command = {{"nearestCompletionKey", 65}, {"manualRouteKey", 66}}; command[field] = malformed;
        RejectsAny([&] { RuntimeHotkeys::Apply(RuntimeHotkeys::ValidateConfiguration(
            command)); },
            "noninteger or invalid JSON binding rejects the entire configuration update");
        Expect(same(RuntimeHotkeys::Snapshot(), defaults), "malformed configuration never partially publishes its valid prefix");
    }
    RuntimeHotkeys::Apply({33, 34, 119, 90, 81});
    Expect(same(RuntimeHotkeys::Snapshot(), {33, 34, 119, 90, 81}), "PageUp/PageDown can be assigned to any action while preserving uniqueness");
    RuntimeHotkeys::Apply(defaults);
    const auto pageOnly = RuntimeHotkeys::ValidateConfiguration({{"guidePreviousImageKey", 65}});
    Expect(same(pageOnly, {90, 81, 119, 65, 34}), "a partial previous-page change preserves the other four keys");
    const auto swapped = RuntimeHotkeys::ValidateConfiguration({{"nearestCompletionKey", 81}, {"manualRouteKey", 90},
        {"guidePreviousImageKey", 34}, {"guideNextImageKey", 33}});
    RuntimeHotkeys::Apply(swapped);
    Expect(same(RuntimeHotkeys::Snapshot(), {81, 90, 119, 34, 33}), "valid swaps publish both page keys and other bindings together");
    std::atomic_bool ready = false, finished = false;
    std::atomic_int torn = 0, reads = 0;
    std::thread reader([&] {
        ready = true;
        do {
            const auto value = RuntimeHotkeys::Snapshot();
            if (!same(value, defaults) && !same(value, swapped)) ++torn;
            ++reads;
        } while (!finished.load());
    });
    while (!ready.load()) std::this_thread::yield();
    for (int i = 0; i < 10000; ++i) RuntimeHotkeys::Apply(i % 2 ? defaults : swapped);
    finished = true; reader.join();
    Expect(reads > 0 && torn == 0, "input threads observe complete old or new five-key snapshots including the high 64-bit page field");
    RuntimeHotkeys::Apply({});
}
void GuidePaginationTests() {
    using Json = nlohmann::json;
    using AutoRoute::EscapeAction;
    // 攻略键：一次按下只产生一次开/关请求。实机报告"按 F8 没反应"，日志里 0.9 秒内出现 10 次
    // guide-shortcut —— 每次都会走一遍 ToggleGuideAsync（开着就关），连按就等于开了又关。
    // 状态机把重复 key-down 归成 Consume（不是 PassThrough），所以"只有 ReturnToPan 才发请求"
    // 这条判断本身就挡住了重复；钩子里那个 firstDown 条件是冗余的显式表达，这里把它钉死。
    {
        // 每个场景用独立实例：状态机是有状态的，前一个场景留下的 pressed_/owned_ 会污染下一个。
        AutoRoute::PlanningEscapeKey first;
        Expect(first.Handle(true, true, true, false) == EscapeAction::ReturnToPan,
            "the first guide-key down asks for one open or close");
        Expect(first.Handle(true, true, true, false) == EscapeAction::Consume,
            "a repeated guide-key down is consumed and asks for nothing");
        Expect(first.Handle(false, true, true, false) == EscapeAction::Consume,
            "the matching key-up is consumed so the game never sees it");

        // 按住不放：Windows 连续送 down 而没有 up，整串只能产生一次请求。
        AutoRoute::PlanningEscapeKey held;
        int requests = 0;
        if (held.Handle(true, true, true, false) == EscapeAction::ReturnToPan) ++requests;
        for (int repeat = 0; repeat < 20; ++repeat)
            if (held.Handle(true, true, true, false) == EscapeAction::ReturnToPan) ++requests;
        Expect(requests == 1, "holding the guide key down stays a single request");
        held.Handle(false, true, true, false);
        Expect(held.Handle(true, true, true, false) == EscapeAction::ReturnToPan,
            "releasing and pressing again is a new request");

        // 攻略窗口打开后游戏失去前台：那几次 down 不能算作请求，也不能把所有权交出去。
        AutoRoute::PlanningEscapeKey unfocused;
        unfocused.Handle(true, true, true, false);
        Expect(unfocused.Handle(true, true, false, false) == EscapeAction::Consume,
            "a held key whose focus moved to the guide is still consumed, not passed to the game");
    }
    const Json first = {{"hwnd", 1234}, {"profileId", "local"}, {"stateId", 8},
        {"pointId", "1409977912641277952"}, {"selectionGeneration", 71}};
    auto next = first; next["pointId"] = "1409980210964680704"; next["selectionGeneration"] = 72;
    auto expected = first; expected["type"] = "markerGuidePageRequested"; expected["direction"] = -1;
    Expect(MarkerGuideProtocol::PageRequest(first, "local", true, false, -1) == expected,
        "game-focused previous-page request preserves the visible guide's complete identity and generation");
    expected["direction"] = 1;
    Expect(MarkerGuideProtocol::PageRequest(first, "local", false, true, 1) == expected,
        "guide-focused next-page request has the same exact identity and a forward direction");
    Expect(MarkerGuideProtocol::PageRequest(Json::object(), "local", true, false, 1).is_null(),
        "hidden guide has no visible registration and cannot consume a game page key");
    Expect(MarkerGuideProtocol::PageRequest({{"hwnd", 1234}}, "local", true, false, 1).is_null(),
        "an overlap chooser without a selected point cannot page an unrelated guide");
    Expect(MarkerGuideProtocol::PageRequest(first, "other-profile", true, false, 1).is_null() &&
        MarkerGuideProtocol::PageRequest(first, "local", false, false, 1).is_null() &&
        MarkerGuideProtocol::PageRequest(first, "local", true, false, 0).is_null(),
        "stale profile, another focused application and an invalid direction cannot produce a page request");

    AutoRoute::PlanningEscapeKey key;
    std::vector<Json> emitted;
    const auto hook = [&](bool down, const Json& visible, bool focused, int direction, bool modifiers = false) {
        const auto request = MarkerGuideProtocol::PageRequest(visible, "local", focused, false, direction);
        const auto action = key.Handle(down, !modifiers && !request.is_null(), focused, false);
        if (action == EscapeAction::ReturnToPan) emitted.push_back(request);
        return action;
    };
    Expect(hook(true, first, true, -1) == EscapeAction::ReturnToPan && emitted.size() == 1,
        "one physical page-key down requests exactly one page");
    Expect(hook(true, next, true, 1) == EscapeAction::Consume && emitted.size() == 1 &&
        emitted.front().at("selectionGeneration") == 71 && emitted.front().at("pointId") == first.at("pointId"),
        "holding or rebinding the page key while the guide switches points cannot emit another page or rewrite the queued identity");
    Expect(hook(true, Json::object(), false, 1) == EscapeAction::Consume &&
        hook(false, Json::object(), false, 1) == EscapeAction::Consume && emitted.size() == 1,
        "owned page repeat and matching release stay consumed after the guide hides or focus changes");
    Expect(hook(true, Json::object(), true, 1) == EscapeAction::PassThrough && emitted.size() == 1,
        "a fresh page press while the guide is hidden belongs to the game");
    Expect(hook(true, next, true, 1) == EscapeAction::PassThrough && emitted.size() == 1,
        "showing the guide cannot take over a page key already held by the game");
    hook(false, next, true, 1);
    Expect(hook(true, next, true, 1) == EscapeAction::ReturnToPan && emitted.size() == 2 &&
        emitted.back().at("selectionGeneration") == 72 && emitted.back().at("direction") == 1,
        "the next fresh page-key press uses only the current guide session");
    hook(false, next, true, 1);
    Expect(hook(true, next, true, 1, true) == EscapeAction::PassThrough &&
        hook(true, next, true, 1, false) == EscapeAction::PassThrough && emitted.size() == 2,
        "modified page shortcuts pass through and releasing the modifier cannot steal the existing press");
}

void MarkerGuideProtocolTests() {
    using Json = nlohmann::json;
    const Json registration = {{"hwnd", 1234}, {"profileId", "local"}, {"stateId", 8},
        {"pointId", "1409977912641277952"}, {"selectionGeneration", 72057594037927937ULL}};
    Expect(MarkerGuideProtocol::Registration(registration, "local") == registration,
        "guide registration retains the full point identity and exact generation above JavaScript integer precision");
    Expect(MarkerGuideProtocol::Registration({{"hwnd", 0}}, "local") == Json({{"hwnd", 0}}),
        "zero HWND without identity unregisters the current guide");
    Expect(MarkerGuideProtocol::Registration({{"hwnd", 1234}}, "local") == Json({{"hwnd", 1234}}),
        "overlap chooser can register only its HWND");
    RejectsAny([&] { MarkerGuideProtocol::Registration(Json::object(), "local"); }, "registration requires an explicit HWND");
    for (const auto* field : {"profileId", "stateId", "pointId", "selectionGeneration"}) {
        auto incomplete = registration; incomplete.erase(field);
        Rejects([&] { MarkerGuideProtocol::Registration(incomplete, "local"); }, "partial guide identity is rejected");
        Rejects([&] { MarkerGuideProtocol::Registration({{"hwnd", 1234}, {field, registration.at(field)}}, "local"); },
            "one identity field cannot register an ambiguous guide");
    }
    for (const Json invalid : std::vector<Json>{-1, 1.5, true, nullptr, "1"}) {
        for (const auto* field : {"hwnd", "stateId", "selectionGeneration"}) {
            auto malformed = registration; malformed[field] = invalid;
            Rejects([&] { MarkerGuideProtocol::Registration(malformed, "local"); },
                "registration does not coerce negative, fractional or nonnumeric identity values");
        }
    }
    for (const auto* field : {"hwnd", "stateId", "selectionGeneration"}) {
        auto zero = registration; zero[field] = 0;
        Rejects([&] { MarkerGuideProtocol::Registration(zero, "local"); }, "an identified guide requires positive HWND, state and generation");
    }
    auto boundary = registration; boundary["stateId"] = 2147483647ULL;
    Expect(MarkerGuideProtocol::Registration(boundary, "local").at("stateId") == 2147483647,
        "largest valid signed state ID survives unsigned JSON representation");
    boundary["stateId"] = 2147483648ULL;
    Rejects([&] { MarkerGuideProtocol::Registration(boundary, "local"); }, "unsigned state ID above INT_MAX cannot wrap to another map");
    Rejects([&] { MarkerGuideProtocol::Registration(registration, "other"); }, "registration from an old profile is rejected");
    for (const Json invalid : std::vector<Json>{"", nullptr, 42, true}) {
        auto malformed = registration; malformed["pointId"] = invalid;
        Rejects([&] { MarkerGuideProtocol::Registration(malformed, "local"); }, "guide identity requires a nonempty opaque string point ID");
    }
    // 放大图片窗口用同一个登记通道，但多一个 `picture` 标记：原生钩子据此决定 ESC 归不归攻略
    // （大图没开时 ESC 必须留给大地图的取消手势）。只接受 true，别的写法都不能凭空造出"大图开着"。
    Expect(MarkerGuideProtocol::Registration(registration, "local").count("picture") == 0,
        "an ordinary guide registration never claims an enlarged picture");
    auto picture = registration; picture["picture"] = true;
    Expect(MarkerGuideProtocol::Registration(picture, "local").value("picture", false),
        "the enlarged picture window registers itself as a picture window");
    auto notPicture = registration; notPicture["picture"] = false;
    Expect(MarkerGuideProtocol::Registration(notPicture, "local") == registration,
        "an explicit false picture flag is the same as an ordinary guide registration");
    for (const Json invalid : std::vector<Json>{1, "true", nullptr, 0}) {
        auto malformed = registration; malformed["picture"] = invalid;
        Expect(MarkerGuideProtocol::Registration(malformed, "local") == registration,
            "a non-boolean picture flag cannot claim the picture routing");
    }
    const Json context = {{"guideSelectionGeneration", 72057594037927937ULL}, {"guideWindowHwnd", 1234}};
    auto command = context; command["pointId"] = "1409977912641277952";
    Expect(MarkerGuideProtocol::CompletionContext(command) == context,
        "completion correlation copies exact generation and HWND without unrelated command fields");
    Expect(MarkerGuideProtocol::CompletionContext(Json::object()).empty(), "ordinary completion needs no guide context");
    for (const auto* field : {"guideSelectionGeneration", "guideWindowHwnd"}) {
        const Json partial = {{field, context.at(field)}};
        Expect(MarkerGuideProtocol::CompletionContext(partial) == partial, "each optional completion correlation field is independently supported");
        for (const Json invalid : std::vector<Json>{0, -1, 1.5, true, nullptr, "1"}) {
            auto malformed = context; malformed[field] = invalid;
            Rejects([&] { MarkerGuideProtocol::CompletionContext(malformed); },
                "invalid completion correlation is rejected before a progress mutation can occur");
        }
    }
}
void AutoReplanTests() {
    using namespace std::chrono_literals;
    const auto now=AutoRoute::ReplanClock::time_point{}+10s;
    AutoRoute::StablePlayer stable;
    AutoRoute::PlayerObservation p{"local",1,1,1,1,{0,0},now,now,true,true};
    stable.Observe(p,now);
    for(int i=0;i<20;++i)stable.Observe(p,now+100ms);
    Expect(!stable.Ready(now+100ms),"repeated presentations cannot fabricate independent localization confirmations");
    p.fixSequence=2;p.fixCapturedAt=p.latestCaptureAt=now+100ms;stable.Observe(p,now+100ms);
    p.fixSequence=3;p.fixCapturedAt=p.latestCaptureAt=now+200ms;stable.Observe(p,now+200ms);
    Expect(stable.Ready(now+200ms),"three independent visual fixes spanning 200ms permit evaluation while moving");
    Expect(!stable.Ready(now+451ms),"freshness also requires a current capture, not just a retained player fix");
    p.visual=false;stable.Observe(p,now+200ms);
    Expect(!stable.Ready(now+200ms),"OCR-only fixes cannot trigger automatic target changes");
    p.visual=true;p.continuityGeneration=2;stable.Observe(p,now+200ms);
    Expect(!stable.Ready(now+200ms),"reacquisition resets confirmation history");
    stable.Reset();p.visual=true;p.continuityGeneration=3;
    for(int i=0;i<4;++i){p.fixSequence=i+1;p.fixCapturedAt=p.latestCaptureAt=now+i*80ms;stable.Observe(p,p.fixCapturedAt);
        Expect(stable.Ready(p.fixCapturedAt)==(i==3),"default 80ms capture cadence becomes stable on frame four at 240ms");}
    p.latestCaptureAt=now+1000ms;
    Expect(!p.Fresh(now+1000ms),"new screenshots cannot renew an old position timestamp");

    AutoRoute::NearbyConfirmation proximity;
    AutoRoute::ProximityObservation q{"local","route","1:new",1,3,1,1,now,now,9.99,true};
    Expect(!proximity.Observe(q,now),"entering the 10px radius does not instantly erase the comparison");
    q.presentedAt=now+100ms;
    Expect(!proximity.Observe(q,now+100ms),"presenting one captured frame repeatedly cannot count as sustained arrival");
    for(int i=1;i<=4;++i){q.sourceFrameId=i+1;q.capturedAt=q.presentedAt=now+i*100ms;
        Expect(!proximity.Observe(q,q.capturedAt),"comparison stays before 500ms of distinct nearby frames");}
    q.sourceFrameId=6;q.capturedAt=q.presentedAt=now+500ms;
    Expect(proximity.Observe(q,q.capturedAt),"500ms of consecutive reliable nearby captures clears only the hint");
    q.distancePixels=10;q.sourceFrameId=7;q.capturedAt=q.presentedAt=now+600ms;
    Expect(!proximity.Observe(q,q.capturedAt)&&!proximity.nearby,"10px itself is outside the original strict completion radius");
    q.distancePixels=1;q.sourceFrameId=8;q.capturedAt=q.presentedAt=now+700ms;proximity.Observe(q,q.capturedAt);
    q.sourceFrameId=9;q.capturedAt=q.presentedAt=now+1000ms;
    Expect(!proximity.Observe(q,q.capturedAt),"a capture gap longer than 250ms restarts arrival confirmation");
    q.valid=false;Expect(!proximity.Observe(q,q.capturedAt)&&!proximity.nearby,"lost localization is never arrival");
    Expect(Near(AutoRoute::TargetDistancePixels({15,0},{0,0},{100,100},1,{1,{-10,0}}),5),
        "nearby projection transforms the target but keeps the HUD player center fixed");
    Expect(Near(AutoRoute::TargetDistancePixels({3,4},{0,0},{100,100},2,{}),10),
        "unfiltered target coordinates use the same captured pixels-per-unit geometry");
    std::string candidate;AutoRoute::ReplanClock::time_point candidateSince{};
    Expect(AutoRoute::CheckTargetSwitch("new",true,9.99,now,{},candidate,candidateSince)=="nearTarget"&&candidate.empty(),
        "a single fresh nearby observation immediately protects the current target from automatic replacement");
    Expect(AutoRoute::CheckTargetSwitch("new",true,10,now,{},candidate,candidateSince)=="confirmingTarget"&&
        AutoRoute::CheckTargetSwitch("new",true,10,now+1999ms,{},candidate,candidateSince)=="confirmingTarget"&&
        AutoRoute::CheckTargetSwitch("new",true,10,now+2s,{},candidate,candidateSince).empty(),
        "target switches require two agreeing plans separated by at least two seconds");
    Expect(AutoRoute::CheckTargetSwitch("other",true,10,now+3s,now,candidate,candidateSince)=="cooldown",
        "target changes respect the eight-second switch cooldown");
    Expect(AutoRoute::CheckTargetSwitch("new",false,10,now+8s,{},candidate,candidateSince)=="waitingForLocation"&&candidate.empty(),
        "missing current proximity evidence discards a pending target switch");

    AutoRoute::Plan original;original.id="preserved";original.name="named route";original.sceneId=1;original.profileId="local";original.start=Origin();
    const auto first=Target("first",100,0),second=Target("second",0,0),done=Target("done",300,0),skipped=Target("skip",400,0);
    original.stops={first,done,skipped,second};original.skipped={AutoRoute::Key(skipped)};original.skipHistory={AutoRoute::Key(skipped)};
    const std::unordered_set<std::string> completed{AutoRoute::Key(done)};
    const auto remaining=AutoRoute::Remaining(original,completed);
    const std::vector<ItemDatas> reordered{second,first};
    Expect(Near(AutoRoute::TargetSpacing(remaining),100),"spacing is derived from real map coordinates without assuming metres");
    Expect(AutoRoute::Worthwhile({0,0},remaining,reordered,100)&&!AutoRoute::Worthwhile({100,0},remaining,reordered,100),
        "both old and new orders are compared from exactly the same current player position");
    const auto merged=AutoRoute::MergeRemaining(original,completed,reordered);
    Expect(merged.id==original.id&&merged.name==original.name&&merged.skipped==original.skipped&&merged.skipHistory==original.skipHistory&&
        Keys(merged.stops)==std::vector<std::string>{AutoRoute::Key(second),AutoRoute::Key(done),AutoRoute::Key(skipped),AutoRoute::Key(first)},
        "replanning preserves all point identities and processed slots including skipped undo history");
    Rejects([&]{AutoRoute::MergeRemaining(original,completed,{second,second});},"duplicate replacement targets are rejected as a whole");
    Rejects([&]{AutoRoute::MergeRemaining(original,completed,{second});},"replanning cannot silently remove an unfinished target");
    Expect(AutoRoute::TargetSpacing({Target("a",0,0),Target("b",0,0)})==0,
        "co-located distinct IDs do not fabricate a distance scale");
    AutoRoute::DrawVisibility visible{"local","preserved","",true};visible.orderRevision=8;visible.comparisonVisible=true;
    RouteDatas segment("route",1);segment.automatic=true;segment.profileId="local";segment.routePlanId="preserved";segment.orderRevision=7;
    Expect(!visible.Allows(segment),"same route ID cannot reuse geometry from an older automatic order");
    segment.orderRevision=8;segment.previousTarget=true;
    Expect(visible.Allows(segment,true),"current comparison geometry may render while navigating");
    visible.comparisonVisible=false;
    Expect(!visible.Allows(segment,true),"cached dashed segments disappear immediately after comparison is cleared");
}
void Benchmark() {
    std::vector<double> elapsed;
    const auto start = Origin();
    for (unsigned trial = 0; trial < 20; ++trial) {
        const auto targets = Sample(AutoRoute::MaxTargets, 5000 + trial);
        const auto began = std::chrono::steady_clock::now();
        const auto result = AutoRoute::Solve(start, targets);
        const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
        elapsed.push_back(milliseconds);
        Expect(result.stops.size() == AutoRoute::MaxTargets && result.planarLength <= result.initialLength + 1e-7,
            "500-target benchmark retains all targets without worsening initial route");
    }
    std::sort(elapsed.begin(), elapsed.end());
    const double p95 = elapsed[static_cast<std::size_t>(std::ceil(elapsed.size() * 0.95)) - 1];
    std::cout << "routePlanner targets=500 trials=20 p95Ms=" << p95 << " maxMs=" << elapsed.back() << '\n';
    Expect(p95 <= 1000, "500-target solve P95 is at most 1 second");
}
}
int main() {
    try { SolverTests(); GeometryTests(); StoreTests(); EscapeOwnershipTests(); DrawingVisibilityTests(); AutoReplanTests(); HotkeyPressOwnershipTests(); GuideHotkeyRoutingTests(); HotkeyConfigurationTests(); GuidePaginationTests(); MarkerGuideProtocolTests(); Benchmark(); }
    catch (const std::exception& error) { ++failures; std::cerr << "UNEXPECTED: " << error.what() << '\n'; }
    if (failures) { std::cerr << failures << " route planning test(s) failed\n"; return 1; }
    std::cout << "Route planning tests passed\n";
    return 0;
}
