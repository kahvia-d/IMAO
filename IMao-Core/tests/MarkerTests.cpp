#include "Runtime/MarkerCompletionStore.h"
#include "Runtime/MarkerLayout.h"
#include <iostream>
#include <stdexcept>
#include <chrono>

using Json = nlohmann::json;
static void Require(bool result, const char* reason) { if (!result) throw std::runtime_error(reason); }
static Json Point(const std::string& id, bool complete = true) {
    return {{"sceneName", "World"}, {"stateId", 8}, {"nameId", "test"}, {"pointId", id}, {"completed", complete}};
}
static Json Send(MarkerCompletionStore& store, std::string type, Json fields = Json::object()) {
    fields["type"] = std::move(type);
    const auto result = store.Execute(fields);
    Require(result.value("accepted", false), result.value("message", "failed").c_str());
    return result.at("data");
}

int main() {
    const auto root = std::filesystem::temp_directory_path() / ("imao-marker-tests-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    std::filesystem::create_directories(root);
    try {
        WriteTextAtomically(root / "account_1.json", R"({"World":{"test":[{"id":"1410302557575794688"}]}})");
        MarkerCompletionStore store(root);
        Require(store.Completed("World", "test", "1410302557575794688"), "legacy string identity lost");
        auto initial = Send(store, "markerGetOutbox");
        Require(initial.at("operations").empty(), "legacy progress must not auto upload");
        auto saved = Send(store, "markerSetCompletion", Point("two"));
        const auto firstRevision = saved.at("point").at("revision");
        auto undone = Send(store, "markerSetCompletion", Point("two", false));
        const auto secondRevision = undone.at("point").at("revision");
        Send(store, "markerAcknowledgeSync", {{"stateId", 8}, {"pointId", "two"}, {"revision", firstRevision}, {"completed", true}});
        auto pending = Send(store, "markerGetOutbox");
        Require(pending.at("operations").size() == 1 && !pending.at("operations")[0].at("completed").get<bool>(), "late ACK cleared undo");
        Send(store, "markerAcknowledgeSync", {{"stateId", 8}, {"pointId", "two"}, {"revision", secondRevision}, {"completed", false}});
        Require(Send(store, "markerGetOutbox").at("operations").empty(), "current ACK did not clear outbox");
        MarkerCompletionStore recovered(root);
        Require(!recovered.Completed("World", "test", "two"), "restart lost undo");
        Require(recovered.Completed("World", "test", "1410302557575794688"), "restart lost legacy progress");
        auto profile = Send(store, "markerSelectProfile", {{"profileId", "cloud_test"}});
        Require(profile.at("points").empty(), "new account inherited local progress");
        Require(!store.Execute({{"type", "markerSetCompletion"}, {"profileId", "local"}, {"stateId", 8},
            {"sceneName", "World"}, {"nameId", "test"}, {"pointId", "x"}, {"completed", true}}).at("accepted").get<bool>(), "stale account write accepted");
        Send(store, "markerCopyLocalProgress", {{"profileId", "cloud_test"}});
        Require(store.Completed("World", "test", "1410302557575794688"), "explicit copy lost local progress");
        auto init = Send(store, "markerInitializeSync", {{"stateId", 8}, {"mode", "upload"},
            {"remoteIds", Json::array({"cloud-only", "unknown-cloud"})}, {"points", Json::array({Point("cloud-only"), Point("untouched", false)})}});
        Require(store.Completed("World", "test", "cloud-only"), "upload deleted untouched cloud completion");
        Require(init.at("syncStates")[0].at("remoteIds").size() == 2, "unknown remote identity lost");
        Require(store.Completed("World", "test", "unknown-cloud"), "remote-only completion was not visible without a local copy");
        auto cloudIds = store.CompletedIds("World", "test");
        Require(std::find(cloudIds.begin(), cloudIds.end(), "unknown-cloud") != cloudIds.end(), "remote-only completion was not exposed to map drawing");
        Require(!store.Execute({{"type", "markerCopyLocalProgress"}}).at("accepted").get<bool>(), "initialized account allowed surprise bulk copy");
        auto unseen = Send(store, "markerSetCompletion", Point("previously-unseen"));
        Require(unseen.at("point").at("remoteCompleted") == false && unseen.at("point").at("pending") == true,
            "initialized unseen point must use false cloud baseline");
        const auto unseenRevision = unseen.at("point").at("revision");
        auto beforeFailure = Send(store, "markerGetSnapshot").at("revision");
        const auto profilePath = root / "profiles" / "cloud_test.json";
        HANDLE locked = CreateFileW(profilePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        Require(locked != INVALID_HANDLE_VALUE, "could not lock fixture");
        auto failedWrite = Point("fail-write"); failedWrite["type"] = "markerSetCompletion";
        auto failure = store.Execute(failedWrite);
        CloseHandle(locked);
        Require(!failure.at("accepted").get<bool>(), "failed disk commit reported success");
        Require(!store.Completed("World", "test", "fail-write") && Send(store, "markerGetSnapshot").at("revision") == beforeFailure,
            "failed commit altered memory or outbox revision");
        Require(!store.Execute({{"type", "markerSelectProfile"}, {"profileId", "../escape"}}).at("accepted").get<bool>(), "profile traversal accepted");
        auto old = Send(store, "markerGetSnapshot", {{"profileId", "local"}});
        Require(old.at("activeProfileId") == "cloud_test", "inactive read disguised active account");
        // Cloud baseline true, pending local false, remote false should not be
        // considered a conflict merely because another client made the same change.
        Send(store, "markerSetCompletion", Point("cloud-only", false));
        auto applied = Send(store, "markerApplyRemote", {{"stateId", 8}, {"remoteIds", Json::array({"unknown-cloud"})}});
        Require(applied.at("conflicts").empty(), "convergent change treated as conflict");
        Require(!store.Completed("World", "test", "cloud-only"), "remote application overwrote pending undo");
        auto cloudUndo = Send(store, "markerGetSnapshot", {{"stateId", 8}, {"pointId", "cloud-only"}}).at("points")[0];
        auto resolved = Send(store, "markerResolveConflict", {{"stateId", 8}, {"pointId", "cloud-only"},
            {"expectedRevision", cloudUndo.at("revision")}, {"completed", true}});
        Require(store.Completed("World", "test", "cloud-only"), "resolve cloud state did not apply");
        Require(!store.Execute({{"type", "markerResolveConflict"}, {"stateId", 8}, {"pointId", "cloud-only"},
            {"expectedRevision", cloudUndo.at("revision")}, {"completed", false}}).at("accepted").get<bool>(), "stale conflict resolution accepted");
        {
            // Preview describes what a sync would change and must leave the
            // profile untouched while doing so.
            const auto previewRoot = root / "preview";
            std::filesystem::create_directories(previewRoot);
            MarkerCompletionStore preview(previewRoot);
            Send(preview, "markerSetCompletion", Point("local-done"));
            const Json supplied = Json::array({Point("cloud-only"), Point("local-done"), Point("untouched", false)});
            const Json cloud = Json::array({"cloud-only", "unmapped-cloud"});
            const auto beforePlan = Send(preview, "markerGetSnapshot");
            auto plan = Send(preview, "markerPreviewSync", {{"stateId", 8}, {"mode", "import"}, {"remoteIds", cloud}, {"points", supplied}});
            const auto region = plan.at("regions").at(0);
            Require(plan.at("regions").size() == 1 && region.at("stateId").get<int>() == 8, "single-region preview must report that region");
            Require(!region.at("initialized").get<bool>(), "preview must report an uninitialized region");
            Require(region.at("willAdd").get<int>() == 1 && region.at("willRemove").get<int>() == 1 && region.at("unchanged").get<int>() == 1,
                "preview must report the expected first-sync changes");
            Require(region.at("remoteCompleted").get<int>() == 1 && region.at("localCompleted").get<int>() == 1,
                "preview must report both sides of the comparison");
            Require(plan.at("remoteCompleted").get<int>() == 2 && plan.at("unmappedRemote").get<int>() == 1,
                "preview must keep account-wide totals and report cloud identities without local points");
            Require(plan.at("unmappedIds").size() == 1 && plan.at("unmappedIds").at(0).get<std::string>() == "unmapped-cloud",
                "preview must name the cloud identities that have no local point");
            Require(Send(preview, "markerGetSnapshot").at("revision") == beforePlan.at("revision"),
                "preview modified the profile revision");
            auto merged = Send(preview, "markerPreviewSync", {{"stateId", 8}, {"mode", "merge"}, {"remoteIds", cloud}, {"points", supplied}});
            Require(merged.at("regions").at(0).at("willAdd").get<int>() == 1 && merged.at("regions").at(0).at("willRemove").get<int>() == 0,
                "merge preview must never plan a cancellation");
            // A cloud identity that belongs to another region must land in that
            // region's row instead of being copied into every region.
            const Json crossSupplied = Json::array({Point("cloud-only"), Point("local-done"), Point("untouched", false),
                {{"sceneName", "Tethys"}, {"stateId", 900}, {"nameId", "test"}, {"pointId", "tethys-done"}, {"completed", false}}});
            const Json crossCloud = Json::array({"cloud-only", "unmapped-cloud", "tethys-done"});
            auto all = Send(preview, "markerPreviewSync", {{"stateId", 0}, {"mode", "import"}, {"remoteIds", crossCloud}, {"points", crossSupplied}});
            Require(all.at("regions").size() == 2, "account-wide preview must group identities by region");
            for (const auto& row : all.at("regions")) {
                const auto state = row.at("stateId").get<int>();
                Require(row.at("remoteCompleted").get<int>() == 1, "grouped preview must scope identities to their own region");
                Require(row.at("remoteIds").at(0).get<std::string>() == (state == 8 ? "cloud-only" : "tethys-done"),
                    "grouped preview must keep the region's own cloud identities");
            }
            Send(preview, "markerInitializeSync", {{"stateId", 8}, {"mode", "import"}, {"remoteIds", cloud}, {"points", supplied}});
            Require(!preview.Completed("World", "test", "local-done"), "import did not apply the previewed removal");
            Require(preview.Completed("World", "test", "cloud-only"), "import did not apply the previewed addition");
            auto steady = Send(preview, "markerPreviewSync", {{"stateId", 8}, {"mode", "import"}, {"remoteIds", cloud}, {"points", supplied}});
            Require(steady.at("regions").at(0).at("initialized").get<bool>() && steady.at("regions").at(0).at("willAdd").get<int>() == 0 &&
                steady.at("regions").at(0).at("willRemove").get<int>() == 0,
                "repeat sync must report no further changes");
            Require(plan.at("regions").at(0).at("bothCompleted").get<int>() == 0 && steady.at("regions").at(0).at("bothCompleted").get<int>() == 1,
                "preview must count the identities both sides already agree on");
            Require(steady.at("localCompleted").get<int>() == 1 && steady.at("remoteCompleted").get<int>() == 2,
                "preview must report both side totals for the comparison header");
        }
        std::vector<MarkerLayoutPoint> points = {{"a", 5, 5, 0}, {"b", 6, 6, 1}, {"c", 150, 150, 2}};
        auto groups = BuildMarkerLayout(points, 30);
        Require(groups.size() == 2 && groups[0].members.size() == 2, "screen overlap grouping failed");
        std::reverse(points.begin(), points.end());
        auto reverse = BuildMarkerLayout(points, 30);
        Require(reverse[0].anchor.key == groups[0].anchor.key && reverse[0].members[1].key == groups[0].members[1].key, "group order unstable");
        Require(groups[0].members[0].x == 5 && groups[0].members[1].x == 6, "layout altered real positions");
        std::vector<MarkerLayoutPoint> dense;
        for (int i = 0; i < 25000; ++i) dense.push_back({std::to_string(i), 0, 0, static_cast<std::size_t>(i)});
        auto started = std::chrono::steady_clock::now();
        auto cluster = BuildMarkerLayout(std::move(dense), 30);
        Require(cluster.size() == 1 && cluster[0].members.size() == 25000, "dense cluster lost markers");
        Require(std::chrono::steady_clock::now() - started < std::chrono::seconds(3), "dense layout exceeded bounded processing budget");
        MarkerClickTracker click;
        click.Down("a", 0, 0); Require(click.Up("a", 1, 1) == "a", "single target click failed");
        click.Down("a", 0, 0); click.Move(30, 0); Require(click.Up("a", 0, 0).empty(), "drag selected a marker");
        click.Down("a", 0, 0); Require(click.Up("b", 0, 0).empty(), "release selected a different marker");
        std::cout << "Marker layout, interaction, account isolation and durable synchronization tests passed\n";
        std::filesystem::remove_all(root);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\nFixture retained at " << root << '\n';
        return 1;
    }
}
