#pragma once
#include "Runtime/MarkerCompletionStore.h"
#include "Runtime/FarmCompletionStore.h"
#include <chrono>
#include <functional>
#include <iostream>
#include <map>
#include <cstdlib>
#include <atomic>
#include <thread>
#include <vector>

namespace CompletionViewTests {
using Json = nlohmann::json;
struct Probe {
    std::string profile;
    std::function<bool(const std::string&, const std::string&)> contains;
};

inline Probe LegacyRead(MarkerCompletionStore& store, const std::string& scene,
    const std::vector<std::string>& categories) {
    std::map<std::string, std::vector<std::string>> ids;
    for (const auto& category : categories) ids[category] = store.CompletedIds(scene, category);
    return {store.Profile(), [ids = std::move(ids)](const auto& category, const auto& id) {
        const auto found = ids.find(category);
        return found != ids.end() && std::find(found->second.begin(), found->second.end(), id) != found->second.end();
    }};
}

// Until the batch reader exists, exercise the actual per-category path. This
// makes the performance regression fail at runtime against today's implementation.
template<class Store>
Probe Read(Store& store, const std::string& scene, const std::vector<std::string>& categories) {
    if (const auto* legacy = std::getenv("IMAO_TEST_LEGACY_COMPLETION_READ"); legacy && std::string(legacy) == "1")
        return LegacyRead(store, scene, categories);
    if constexpr (requires { store.ReadCompletions(scene); }) {
        auto view = store.ReadCompletions(scene);
        auto profile = view.profileId;
        return {profile, [view = std::move(view)](const auto& category, const auto& id) { return view.Contains(category, id); }};
    } else return LegacyRead(store, scene, categories);
}

template<class Store>
Probe ReadFarm(Store& store, int state) {
    if constexpr (requires { store.ReadCompletions(state); }) {
        auto view = store.ReadCompletions(state);
        auto profile = view.profileId;
        return {profile, [view = std::move(view)](const auto&, const auto& id) { return view.Contains(id); }};
    } else {
        auto ids = store.CompletedIds(state);
        return {"", [ids = std::move(ids)](const auto&, const auto& id) {
            return std::find(ids.begin(), ids.end(), id) != ids.end();
        }};
    }
}

inline Json Point(const std::string& scene, int state, const std::string& category,
    const std::string& id, bool completed) {
    return {{"sceneName", scene}, {"stateId", state}, {"nameId", category}, {"pointId", id},
        {"completed", completed}, {"remoteCompleted", nullptr}, {"pending", false}, {"revision", 1}};
}

template<class Check>
void Test(const std::filesystem::path& root, Check check) {
    const auto fixture = root / "completion-view";
    Json document = {{"schemaVersion", 2}, {"profileId", "local"}, {"revision", 1},
        {"points", Json::object()}, {"syncStates", Json::array({
            {{"stateId", 8}, {"initialized", true}, {"remoteIds", Json::array({"remote-only", "undone", "both", ""})}},
            {{"stateId", 902}, {"initialized", true}, {"remoteIds", Json::array({"vault-remote"})}}})}};
    document["points"]["8:undone"] = Point("World", 8, "alpha", "undone", false);
    document["points"]["8:both"] = Point("World", 8, "beta", "both", false);
    document["points"]["8:local-only"] = Point("World", 8, "alpha", "local-only", true);
    document["points"]["902:vault-local"] = Point("LowerVault", 902, "alpha", "vault-local", true);
    // Preserve the existing map-filter semantics: sceneName/nameId, even for a
    // layered state distinct from SceneState(scene). Do not substitute Completed().
    document["points"]["909:layered"] = Point("World", 909, "beta", "layered", true);
    WriteTextAtomically(fixture / "profiles/local.json", document.dump());
    MarkerCompletionStore store(fixture);
    const std::vector<std::string> categories{"alpha", "beta", "missing", ""};
    const std::vector<std::string> ids{"remote-only", "undone", "both", "local-only", "vault-remote", "vault-local", "layered", "unknown", ""};
    const auto equivalent = [&](const std::string& scene) {
        auto view = Read(store, scene, categories);
        check(view.profile == store.Profile(), "completion read pins its account identity");
        for (const auto& category : categories) {
            const auto expected = store.CompletedIds(scene, category);
            for (const auto& id : ids)
                check(view.contains(category, id) == (std::find(expected.begin(), expected.end(), id) != expected.end()),
                    "batch completion membership equals the existing map-filter result");
        }
    };
    for (const auto& scene : {"World", "LowerVault", "unknown"}) equivalent(scene);
    auto before = Read(store, "World", categories);
    check(!before.contains("alpha", "undone") && before.contains("beta", "undone"),
        "local undo only overrides its category's remote baseline");
    auto change = Point("World", 8, "alpha", "undone", true);
    change["type"] = "markerSetCompletion";
    check(store.Execute(change).value("accepted", false), "completion update accepted");
    check(!before.contains("alpha", "undone") && Read(store, "World", categories).contains("alpha", "undone"),
        "a new frame sees a local change without mutating the previous frame's read");
    check(store.Execute({{"type", "markerApplyRemote"}, {"stateId", 8}, {"remoteIds", Json::array({"new-remote"})},
        {"points", Json::array()}}).value("accepted", false), "remote baseline update accepted");
    check(Read(store, "World", categories).contains("alpha", "new-remote"), "a new frame sees a cloud baseline change");
    equivalent("World");
    check(store.Execute({{"type", "markerSelectProfile"}, {"profileId", "other"}}).value("accepted", false), "account switch accepted");
    auto other = Read(store, "World", categories);
    check(other.profile == "other" && !other.contains("alpha", "local-only"), "a new account never inherits the previous account's progress");
    check(before.profile == "local" && before.contains("alpha", "local-only"), "retained reads stay bound to their original account");

    const auto perfRoot = root / "completion-view-performance";
    document["points"] = Json::object();
    document["syncStates"] = Json::array({{{"stateId", 8}, {"initialized", true}, {"remoteIds", Json::array()}}});
    std::vector<std::string> manyCategories;
    for (int c = 0; c < 24; ++c) manyCategories.push_back("category-" + std::to_string(c));
    for (int i = 0; i < 12000; ++i) {
        const auto id = std::to_string(1300000000000000000LL + i);
        document["syncStates"][0]["remoteIds"].push_back(id);
        document["points"]["8:" + id] = Point("World", 8, manyCategories[i % manyCategories.size()], id, i % 3 != 0);
    }
    WriteTextAtomically(perfRoot / "profiles/local.json", document.dump());
    MarkerCompletionStore large(perfRoot);
    std::vector<double> legacyTimes, batchTimes;
    const auto measure = [&](auto reader) {
        const auto start = std::chrono::steady_clock::now();
        auto view = reader();
        check(view.contains(manyCategories[1], "1300000000000000001"), "performance fixture preserves completed identity");
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };
    for (int n = 0; n < 5; ++n) {
        legacyTimes.push_back(measure([&] { return LegacyRead(large, "World", manyCategories); }));
        batchTimes.push_back(measure([&] { return Read(large, "World", manyCategories); }));
    }
    std::sort(legacyTimes.begin(), legacyTimes.end()); std::sort(batchTimes.begin(), batchTimes.end());
    std::cout << "Completion read benchmark categories=24 records=12000 legacyMs=" << legacyTimes[2]
        << " batchMs=" << batchTimes[2] << " ratio=" << batchTimes[2] / legacyTimes[2] << '\n';
    check(batchTimes[2] < legacyTimes[2] * 0.25, "many-category completion reads must remove repeated whole-ledger scans (at least 4x faster)");

    std::int64_t day = 100;
    FarmCompletionStore farm(root / "farm-view", [&] { return day; });
    farm.SetMany({{8, "farm-world", true}, {902, "farm-vault", true}});
    const auto farmBefore = ReadFarm(farm, 8);
    check(farmBefore.profile == "local", "farm reads must atomically pin the account with their completion set");
    check(farmBefore.contains("", "farm-world") && !farmBefore.contains("", "farm-vault"), "farm reads isolate scene identities");
    farm.Set(8, "farm-world", false);
    check(farmBefore.contains("", "farm-world") && !ReadFarm(farm, 8).contains("", "farm-world"), "a farm undo is visible on the next frame only");
    farm.Set(8, "daily", true);
    const auto beforeDay = ReadFarm(farm, 8);
    ++day;
    check(beforeDay.contains("", "daily") && !ReadFarm(farm, 8).contains("", "daily"), "a new frame observes the daily boundary without a persistent cached set");
    farm.Set(8, "local-farm", true);
    farm.SelectProfile("other");
    auto farmOther = ReadFarm(farm, 8);
    check(farmOther.profile == "other" && !farmOther.contains("", "local-farm"), "farm account switches never inherit another account's progress");
    check(!ReadFarm(farm, 0).contains("", "daily"), "an invalid farm scene exposes no completions");

    PointFilterCompletionRead routed;
    routed.permanent.profileId = "local";
    routed.farm.profileId = "local";
    routed.permanent.remoteIds = {"daily-old-cloud", "collectible"};
    routed.dailyCategories = {"harvest", "enemy"};
    check(routed.Compatible() && !routed.Contains("harvest", "daily-old-cloud") &&
        routed.Contains("collectible-category", "collectible"),
        "daily expiry cannot be undone by a cloud baseline and cannot erase permanent collections");
    routed.farm.ids.insert("daily-new");
    check(routed.Contains("enemy", "daily-new") && !routed.Contains("collectible-category", "daily-new"),
        "daily and permanent completion identities remain in separate ledgers");
    routed.farm.profileId = "other";
    check(!routed.Compatible() && !routed.Contains("enemy", "daily-new") &&
        !routed.Contains("collectible-category", "collectible"),
        "a cross-account ledger handoff cannot authorize a mixed completion read");

    // The two stores switch separately in the runtime. A concurrent frame may
    // see either complete account, or an incompatible pair, never an accepted mix.
    const auto raceRoot = root / "completion-profile-handoff";
    for (const auto& profile : {"local", "other"}) {
        Json account = {{"schemaVersion", 2}, {"profileId", profile}, {"revision", 1},
            {"points", Json::object()}, {"syncStates", Json::array()}};
        account["points"]["8:" + std::string(profile)] = Point("World", 8, "collectible", profile, true);
        WriteTextAtomically(raceRoot / "profiles" / (std::string(profile) + ".json"), account.dump());
    }
    MarkerCompletionStore switching(raceRoot);
    FarmCompletionStore switchingFarm(root / "farm-profile-handoff", [&] { return day; });
    switchingFarm.Set(8, "local", true);
    switchingFarm.SelectProfile("other"); switchingFarm.Set(8, "other", true); switchingFarm.SelectProfile("local");
    std::atomic_bool done = false, inconsistent = false;
    std::thread writer([&] {
        try {
            for (int n = 0; n < 200; ++n) {
                const auto profile = n % 2 ? "local" : "other";
                if (!switching.Execute({{"type", "markerSelectProfile"}, {"profileId", profile}}).value("accepted", false))
                    inconsistent = true;
                switchingFarm.SelectProfile(profile);
                std::this_thread::yield();
            }
        } catch (...) { inconsistent = true; }
        done = true;
    });
    try {
        for (int n = 0; n < 300 || !done; ++n) {
            PointFilterCompletionRead frame;
            frame.permanent = switching.ReadCompletions("World");
            frame.farm = switchingFarm.ReadCompletions(8);
            frame.dailyCategories = {"daily"};
            if (!frame.Compatible()) {
                if (frame.Contains("daily", "local") || frame.Contains("collectible", "other")) inconsistent = true;
                continue;
            }
            const auto& profile = frame.permanent.profileId;
            const auto otherProfile = profile == "local" ? "other" : "local";
            if (!frame.Contains("collectible", profile) || !frame.Contains("daily", profile) ||
                frame.Contains("collectible", otherProfile) || frame.Contains("daily", otherProfile)) inconsistent = true;
        }
    } catch (...) { writer.join(); throw; }
    writer.join();
    check(!inconsistent, "concurrent account handoffs never accept a mixed permanent/daily completion read");
}
}
