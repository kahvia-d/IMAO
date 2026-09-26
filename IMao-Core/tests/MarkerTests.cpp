#include "Runtime/MarkerCompletionStore.h"
#include "Runtime/MarkerLayout.h"
#include <iostream>
#include <iterator>
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
static std::string ReadText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
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
        {
            // Docs/LocalAccounts_20260926.md §6.3: an account-era profile can already
            // carry a baseline for a region while a point completed locally was never
            // uploaded. The union flow must keep that point and queue it, not cancel
            // it; only a completion the cloud actually withdrew — the baseline says it
            // had the point — is applied. An explicit import stays cloud-authoritative.
            const auto mergeRoot = root / "merge-baseline";
            std::filesystem::create_directories(mergeRoot / "profiles");
            const auto oldProfile = [](const std::string& profile) {
                return Json{
                    {"schemaVersion", 2}, {"profileId", profile}, {"revision", 4},
                    {"points", {{"8:never-uploaded", {{"sceneName", "World"}, {"nameId", "test"}, {"stateId", 8},
                                    {"pointId", "never-uploaded"}, {"completed", true}, {"pending", false}, {"remoteCompleted", nullptr}}},
                                {"8:withdrawn", {{"sceneName", "World"}, {"nameId", "test"}, {"stateId", 8},
                                    {"pointId", "withdrawn"}, {"completed", true}, {"pending", false}, {"remoteCompleted", true}}}}},
                    {"syncStates", Json::array({{{"stateId", 8}, {"initialized", true}, {"enabled", true},
                        {"initialMode", "merge"}, {"remoteIds", Json::array()}}})}};
            };
            WriteTextAtomically(mergeRoot / "profiles" / "legacy_merge.json", oldProfile("legacy_merge").dump(2));
            WriteTextAtomically(mergeRoot / "profiles" / "legacy_import.json", oldProfile("legacy_import").dump(2));
            MarkerCompletionStore legacy(mergeRoot);
            Send(legacy, "markerSelectProfile", {{"profileId", "legacy_merge"}});
            const auto plan = Send(legacy, "markerPreviewSync", {{"stateId", 8}, {"mode", "merge"}, {"remoteIds", Json::array()}});
            const auto& row = plan.at("regions").at(0);
            Require(row.at("willQueue").get<int>() == 1 && row.at("willRemove").get<int>() == 1,
                "a merge preview must separate a queued local completion from a withdrawn one");
            auto appliedMerge = Send(legacy, "markerApplyRemote", {{"stateId", 8}, {"mode", "merge"}, {"remoteIds", Json::array()}});
            Require(appliedMerge.at("conflicts").empty(), "a merge over an old baseline must not report a conflict");
            Require(legacy.Completed("World", "test", "never-uploaded"), "merge cancelled a completion the cloud never had");
            Require(Send(legacy, "markerGetSnapshot", {{"stateId", 8}, {"pointId", "never-uploaded"}}).at("points").at(0).at("pending").get<bool>(),
                "a kept local completion must be queued for upload");
            Require(!legacy.Completed("World", "test", "withdrawn"), "merge ignored a withdrawal the baseline recorded");
            const auto settledRevision = Send(legacy, "markerGetSnapshot").at("revision");
            Send(legacy, "markerApplyRemote", {{"stateId", 8}, {"mode", "merge"}, {"remoteIds", Json::array()}});
            Require(Send(legacy, "markerGetSnapshot").at("revision") == settledRevision,
                "a settled region must not be rewritten by a repeated merge");
            Send(legacy, "markerSelectProfile", {{"profileId", "legacy_import"}});
            Send(legacy, "markerApplyRemote", {{"stateId", 8}, {"mode", "import"}, {"remoteIds", Json::array()}});
            Require(!legacy.Completed("World", "test", "never-uploaded"), "an explicit import must still follow the cloud");
        }
        {
            // Docs/LocalAccounts_20260926.md section 5.4: the explicit import brings the
            // pre-rewrite record into the ledger the player chose and queues those
            // completions for upload. It is the only route an account ledger has to that
            // file, because loading imports it into "local" alone. Importing twice imports
            // nothing, and the original file is never modified.
            const auto importRoot = root / "legacy-import";
            std::filesystem::create_directories(importRoot);
            const std::string legacyText =
                R"({"World":{"cx_03":[{"id":"1523072790818045952"},{"id":"1523072790818045953"}]},"Tethys":{"cx_03":[{"id":"9001"}]},"NotAScene":{"cx_03":[{"id":"5"}]}})";
            WriteTextAtomically(importRoot / "account_1.json", legacyText);
            MarkerCompletionStore imported(importRoot);
            Send(imported, "markerSelectProfile", {{"profileId", "kuro_910000000001"}});
            Require(!imported.Completed("World", "cx_03", "1523072790818045952"),
                "an account ledger never inherits the old record while loading");
            const auto first = Send(imported, "markerImportLegacyProgress");
            Require(first.at("imported").get<int>() == 3 && first.at("alreadyCompleted").get<int>() == 0 &&
                first.at("skipped").get<int>() == 0,
                "the explicit import brings every known-scene completion into the chosen ledger");
            Require(imported.Completed("World", "cx_03", "1523072790818045952") && imported.Completed("Tethys", "cx_03", "9001") &&
                !imported.Completed("NotAScene", "cx_03", "5"), "an unknown scene stays out of the import");
            Require(Send(imported, "markerGetOutbox").at("operations").size() == 3,
                "imported completions are queued for upload instead of waiting around to be cancelled");
            const auto importedPoint = Send(imported, "markerGetSnapshot", {{"stateId", 8}, {"pointId", "1523072790818045952"}})
                .at("points").at(0);
            Require(importedPoint.at("pending").get<bool>() && importedPoint.at("localTouched").get<bool>(),
                "an imported point carries the upload identity the synchronization needs");
            const auto second = Send(imported, "markerImportLegacyProgress");
            Require(second.at("imported").get<int>() == 0 && second.at("alreadyCompleted").get<int>() == 3,
                "importing the same record twice imports nothing");
            Require(ReadText(importRoot / "account_1.json") == legacyText, "the old record file is never modified");
            MarkerCompletionStore withoutRecord(root / "no-legacy-record");
            Require(withoutRecord.Execute({{"type", "markerImportLegacyProgress"}}).value("message", "") == "no-legacy-record",
                "importing without an old record is refused with a reason");
            Require(imported.Execute({{"type", "markerImportLegacyProgress"}, {"profileId", "kuro_910000000004"}}).value("message", "") == "profile-mismatch",
                "only the ledger the map shows can import into itself");
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
        // A pile of markers from several floors must draw the point on the player's own floor, not
        // whichever key sorts first: the anchor decides the icon and the up/down badge, and the
        // player has to be able to see that something of theirs is in the pile. The current floor's
        // marker here carries the LARGER key, so only the priority can make it the anchor.
        {
            std::vector<MarkerLayoutPoint> floors = {{"8:aaa", 100, 100, 0, 0}, {"9:zzz", 102, 101, 1, 1}};
            auto pile = BuildMarkerLayout(floors, 30);
            Require(pile.size() == 1 && pile[0].members.size() == 2, "layered pile must be one group");
            Require(pile[0].anchor.key == "9:zzz", "the current floor's marker must own the pile's icon");
            std::vector<MarkerLayoutPoint> noPriority = {{"8:aaa", 100, 100, 0, 0}, {"9:zzz", 102, 101, 1, 0}};
            auto byKey = BuildMarkerLayout(noPriority, 30);
            Require(byKey[0].anchor.key == "8:aaa", "without a priority the key order still decides");
        }
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
