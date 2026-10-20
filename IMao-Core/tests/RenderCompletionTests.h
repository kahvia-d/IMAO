#pragma once
#include "Runtime/MarkerCompletionStore.h"
#include "Runtime/FarmCompletionStore.h"
#include "Runtime/FreePointCompletionStore.h"
#include "Runtime/IsolationSwitches.h"
#include "Runtime/OverlayPacing.h"
#include <functional>
#include <type_traits>
#include <cstdlib>

namespace RenderCompletionTests {
using Json = nlohmann::json;
struct Probe {
    std::string profile;
    std::function<bool(const std::string&, const std::string&)> contains;
};

template<class Read>
const void* RoutingStorage(const Read& read) {
    if constexpr (requires { read.dailyRouting; }) return read.dailyRouting.get();
    else return &read.dailyPointIds;
}

template<class Read>
void SetRouting(Read& read, std::unordered_set<std::string> categories, std::unordered_set<std::string> ids) {
    if constexpr (requires { read.dailyRouting; }) {
        using Routing = typename decltype(read.dailyRouting)::element_type;
        read.dailyRouting = std::make_shared<Routing>(Routing{std::move(categories), std::move(ids)});
    } else {
        read.dailyCategories = std::move(categories); read.dailyPointIds = std::move(ids);
    }
}

template<class Store>
Probe Read(Store& store, const std::string& scene) {
    if constexpr (requires { store.ReadRenderCompletions(scene); }) {
        if (!std::getenv("IMAO_TEST_LEGACY_RENDER_COMPLETIONS")) {
            auto view = store.ReadRenderCompletions(scene);
            auto profile = view.profileId;
            return {profile, [view = std::move(view)](const auto& category, const auto& id) {
                return view.Contains(category, id);
            }};
        }
    }
    return {store.Profile(), [&store, scene](const auto& category, const auto& id) {
        return store.Completed(scene, category, id);
    }};
}

template<class Policy>
bool WantsHook(Policy policy, bool interactive, OverlayPacing::Clock::time_point at,
    OverlayPacing::Clock::time_point now, bool dragging, bool disabled) {
    if constexpr (std::is_invocable_r_v<bool, Policy, bool, decltype(at), decltype(now), bool, bool>)
        return policy(interactive, at, now, dragging, disabled);
    else return policy(interactive, at, now, dragging);
}

template<class Check>
void Test(const std::filesystem::path& root, Check check) {
    const auto fixture = root / "render-completions";
    Json doc = {{"schemaVersion", 2}, {"profileId", "local"}, {"revision", 1},
        {"points", Json::object()}, {"syncStates", Json::array({
            {{"stateId", 8}, {"initialized", true}, {"remoteIds", Json::array({"remote", "undone", "renamed"})}},
            {{"stateId", 912}, {"initialized", true}, {"remoteIds", Json::array({"dream"})}}})}};
    const auto point = [](const std::string& scene, int state, const std::string& name,
        const std::string& id, bool complete) {
        return Json{{"sceneName", scene}, {"stateId", state}, {"nameId", name}, {"pointId", id}, {"completed", complete}};
    };
    doc["points"]["8:undone"] = point("World", 8, "alpha", "undone", false);
    doc["points"]["8:renamed"] = point("World", 8, "alpha", "renamed", true);
    // The render API uses the state's identity, unlike category filtering by sceneName.
    doc["points"]["8:state-identity"] = point("legacy-scene", 8, "alpha", "state-identity", true);
    doc["points"]["900:other-state"] = point("World", 900, "alpha", "other-state", true);
    WriteTextAtomically(fixture / "profiles/local.json", doc.dump());
    MarkerCompletionStore store(fixture);
    auto view = Read(store, "World");
    for (const auto& category : {"alpha", "beta"}) for (const auto& id : {
        "remote", "undone", "renamed", "state-identity", "other-state", "dream", "missing"})
        check(view.contains(category, id) == store.Completed("World", category, id),
            "render snapshot preserves per-point category and state semantics");
    check(!view.contains("beta", "renamed"), "a local category mismatch must not fall through to cloud completion");
    auto result = store.Execute({{"type", "markerSetCompletion"}, {"profileId", "local"},
        {"sceneName", "World"}, {"stateId", 8}, {"nameId", "alpha"}, {"pointId", "remote"}, {"completed", false}});
    check(result.value("accepted", false), "render fixture undo accepted");
    check(view.contains("alpha", "remote") && !Read(store, "World").contains("alpha", "remote"),
        "one render frame owns its completion values while the next observes undo");
    store.Execute({{"type", "markerSelectProfile"}, {"profileId", "other"}});
    check(view.profile == "local" && Read(store, "World").profile == "other" &&
        !Read(store, "World").contains("alpha", "renamed"), "render reads keep account identity through a switch");

    std::int64_t day = 100;
    FarmCompletionStore farm(root / "render-farm", [&] { return day; });
    farm.Set(8, "daily", true); farm.Set(912, "dream-daily", true);
    const auto farmBefore = farm.ReadRenderCompletions();
    check(farmBefore.Contains(8, "daily") == farm.Completed(8, "daily") &&
        farmBefore.Contains(912, "dream-daily") == farm.Completed(912, "dream-daily") &&
        !farmBefore.Contains(8, "dream-daily"), "render daily reads preserve point state identity");
    farm.Set(8, "daily", false);
    check(farmBefore.Contains(8, "daily") && !farm.ReadRenderCompletions().Contains(8, "daily"),
        "a render daily snapshot remains owned across an undo");
    ++day;
    check(!farm.ReadRenderCompletions().Contains(912, "dream-daily"), "next render frame observes the daily reset");

    FreePointCompletionStore freeStore(root / "render-free", [&] { return day; });
    ItemDatas freePoint;
    freePoint.layer.stateId = 8; freePoint.layer.stopKind = StopKind::Free;
    freePoint.itemId = "free-point"; freePoint.freeRouteId = "route-a";
    freePoint.freeCategory = FreePointCategory::Daily;
    auto permanentFree = freePoint;
    permanentFree.itemId = "permanent-free"; permanentFree.freeCategory = FreePointCategory::Collectible;
    freeStore.Set("local", freePoint, true); freeStore.Set("local", permanentFree, true);
    const auto freeBefore = freeStore.ReadCompletions("local");
    check(freeBefore.Contains(freePoint) == freeStore.Completed("local", freePoint) &&
        freeBefore.Contains(permanentFree) == freeStore.Completed("local", permanentFree),
        "free render snapshots preserve daily and collectible identities");
    freeStore.Set("local", freePoint, false);
    check(freeBefore.Contains(freePoint) && !freeStore.ReadCompletions("local").Contains(freePoint),
        "free-point undo is visible on the next render frame");
    freeStore.Set("local", freePoint, true); ++day;
    auto freeAfterDay = freeStore.ReadCompletions("local");
    check(!freeAfterDay.Contains(freePoint) && freeAfterDay.Contains(permanentFree),
        "daily free completion expires without clearing permanent free completion");
    check(!freeStore.ReadCompletions("other").Contains(permanentFree) &&
        freeStore.ReadCompletions("local").Contains(permanentFree), "free render reads isolate accounts");

    store.Execute({{"type", "markerSelectProfile"}, {"profileId", "local"}});
    PointRenderCompletionRead combined;
    combined.permanent = store.ReadRenderCompletions("World");
    combined.farm = farm.ReadRenderCompletions();
    combined.free = freeStore.ReadCompletions("local");
    SetRouting(combined, {"harvest"}, {"remote"});
    auto nextFrame = combined;
    check(RoutingStorage(nextFrame) == RoutingStorage(combined),
        "render frames share immutable daily routing instead of allocating the entire catalog each frame");
    SetRouting(combined, {}, {});
    ItemDatas routed; routed.nameId = "alpha"; routed.itemId = "remote";
    nextFrame.permanent.remoteIds.insert("remote");
    check(!nextFrame.Contains(routed), "an owned render frame keeps its routing when the resource owner replaces its catalog");
    SetRouting(combined, {"harvest"}, {"remote"});
    ItemDatas catalog; catalog.nameId = "alpha"; catalog.itemId = "renamed";
    check(combined.Compatible("local") && combined.Contains(catalog), "catalog rendering uses the permanent ledger");
    catalog.itemId = "remote";
    combined.permanent.remoteIds.insert("remote");
    check(!combined.Contains(catalog), "daily point-id routing cannot inherit a cloud completion");
    combined.farm.identities.insert("912:remote"); catalog.layer.stateId = 912;
    check(combined.Contains(catalog), "daily rendering uses the item's state rather than the scene fallback");
    catalog.layer.stateId = 0;
    check(!combined.Contains(catalog), "daily rendering uses the scene state when the item has none");
    check(combined.Contains(permanentFree), "free rendering uses its route ledger instead of official point ids");
    combined.farm.profileId = "other";
    check(!combined.Compatible("local") && !combined.Contains(permanentFree), "mixed account render snapshots authorize no completion");

    using Clock = OverlayPacing::Clock;
    const auto now = Clock::now();
    check(Isolation::Set(512) == 512, "diagnostic mask retains the mouse-only isolation bit");
    check(std::string(Isolation::DescribeAscii(512)) == "no-marker-mouse-hook", "mouse-only isolation is named in diagnostics");
    check(!WantsHook(&OverlayPacing::WantsMouseHook, true, now, now, false, true),
        "mouse-only diagnostic removes a fresh idle hook without changing the map");
    check(WantsHook(&OverlayPacing::WantsMouseHook, false, {}, now, true, true),
        "diagnostic waits for an owned drag release instead of orphaning its mouse-up");
    check(WantsHook(&OverlayPacing::WantsMouseHook, true, now, now, false, false),
        "normal mouse capture resumes when isolation is disabled");
    check(!OverlayPacing::PlanMouseHook(true, now, now, false, true, true).wanted,
        "controller lasso cannot reinstall the diagnostically disabled mouse hook");
    const auto owned = OverlayPacing::PlanMouseHook(true, now, now, true, true, true);
    const auto released = OverlayPacing::PlanMouseHook(true, now, now, false, false, true);
    check(owned.wanted && !released.wanted && !released.discardGestureState,
        "isolation consumes an owned mouse-up and preserves its pending lasso commit");
    check(!OverlayPacing::PlanMouseHook(true, now, now, false, true, true).discardGestureState,
        "mouse-only isolation preserves a controller lasso already in progress");
    check(OverlayPacing::PlanMouseHook(false, {}, now, false, false, false).discardGestureState,
        "ordinary hook removal still discards stale pending input");
    Isolation::Set(0);

    const auto perfRoot = root / "render-completion-performance";
    PointRenderCompletionRead routingRead;
    std::unordered_set<std::string> routingIds;
    for (int n = 0; n < 18699; ++n) routingIds.insert("daily-point-identity-" + std::to_string(n));
    SetRouting(routingRead, {"harvest"}, std::move(routingIds));
    const auto routingStarted = std::chrono::steady_clock::now();
    for (int frame = 0; frame < 120; ++frame) {
        auto owned = routingRead;
        check(RoutingStorage(owned) != nullptr, "render routing remains owned while a frame reads it");
    }
    std::cout << "render-routing frames=120 points=18699 elapsedMs=" <<
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - routingStarted).count() << '\n';
    // The pre-optimization frame copied and destroyed this catalog by value.
    const auto legacyRoutingStarted = std::chrono::steady_clock::now();
    std::size_t routedPoints = 0;
    for (int frame = 0; frame < 120; ++frame) {
        auto owned = *routingRead.dailyRouting;
        routedPoints += owned.pointIds.size();
    }
    check(routedPoints == 120 * 18699, "routing comparison retains the full catalog");
    std::cout << "render-routing legacy-copy frames=120 points=18699 elapsedMs=" <<
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - legacyRoutingStarted).count() << '\n';
    doc["points"] = Json::object();
    doc["syncStates"] = Json::array({{{"stateId", 8}, {"initialized", true}, {"remoteIds", Json::array()}}});
    for (int n = 0; n < 30000; ++n) doc["syncStates"][0]["remoteIds"].push_back("render-id-" + std::to_string(n));
    WriteTextAtomically(perfRoot / "profiles/local.json", doc.dump());
    MarkerCompletionStore large(perfRoot);
    const auto run = [&](bool batch) {
        const auto started = std::chrono::steady_clock::now();
        int completed = 0;
        for (int frame = 0; frame < 6; ++frame) {
            auto read = batch ? Read(large, "World") : Probe{"local", [&large](const auto& cat, const auto& id) {
                return large.Completed("World", cat, id);
            }};
            for (int n = 0; n < 300; ++n) completed += read.contains("alpha", "render-id-" + std::to_string(29700 + n));
        }
        check(completed == 1800, "large synced progress keeps every rendered completion");
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    };
    const double legacyMs = run(false), batchMs = run(true);
    std::cout << "render-completion legacyMs=" << legacyMs << " batchMs=" << batchMs << '\n';
    check(batchMs * 4 < legacyMs, "render-frame completion read avoids a remote-list scan for every marker");
}
}
