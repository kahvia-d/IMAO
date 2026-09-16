#pragma once
#include "AtomicFile.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>
#include <set>

// Completion state and its durable outbox are one atomic document.  The pipe
// only wakes a consumer; losing a notification cannot lose a user's change.
class MarkerCompletionStore {
public:
    using Json = nlohmann::json;
    explicit MarkerCompletionStore(std::filesystem::path directory) : root(std::move(directory)) {
        document = Load("local");
    }

    static int SceneState(const std::string& scene) {
        if (scene == "World") return 8;
        if (scene == "Tethys") return 900;
        if (scene == "Fabricatorium") return 905;
        if (scene == "Avinoleum") return 903;
        if (scene == "Lahai") return 906;
        if (scene == "LowerVault") return 902;
        if (scene == "Darkplain") return 909;
        if (scene == "TimeRiftRuins") return 910;
        return 0;
    }

    Json Execute(const Json& command) {
        std::scoped_lock lock(mutex);
        try {
            const auto type = command.value("type", "");
            const std::string profile = command.value("profileId", document.value("profileId", "local"));
            ValidateProfile(profile);
            if (type == "markerSelectProfile") {
                auto next = Load(profile); // A failed load leaves the old profile active.
                document = std::move(next);
                return Result(Snapshot(document, command));
            }
            const bool readOnly = type == "markerGetSnapshot" || type == "markerGetOutbox";
            if (readOnly) {
                const auto source = profile == document.value("profileId", "local") ? document : Load(profile);
                if (type == "markerGetSnapshot") {
                    auto snapshot = Snapshot(source, command);
                    snapshot["activeProfileId"] = document.at("profileId");
                    return Result(snapshot);
                }
                Json operations = Json::array();
                for (const auto& point : source.at("points")) {
                    if (command.contains("stateId") && command.at("stateId") != point.at("stateId")) continue;
                    if (point.value("pending", false)) operations.push_back(point);
                }
                const auto total = operations.size();
                const auto offset = command.value("offset", std::size_t{});
                const auto limit = std::min<std::size_t>(500, command.value("limit", std::size_t{500}));
                Json page = Json::array();
                for (auto index = offset; index < total && index < offset + limit; ++index) page.push_back(operations[index]);
                return Result({{"profileId", profile}, {"activeProfileId", document.at("profileId")}, {"revision", source.at("revision")}, {"operations", page},
                    {"total", total}, {"hasMore", offset + page.size() < total}, {"syncStates", source.at("syncStates")}});
            }
            if (profile != document.value("profileId", "local")) return Failure("profile-mismatch");
            if (type != "markerResolveConflict" && command.contains("expectedRevision") && command.at("expectedRevision") != document.at("revision"))
                return Failure("revision-conflict");
            Json next = document;
            if (type == "markerCopyLocalProgress") {
                if (profile == "local") return Failure("already-local-profile");
                for (const auto& state : next.at("syncStates"))
                    if (state.value("initialized", false)) return Failure("sync-already-initialized");
                const auto local = Load("local");
                const auto revision = Advance(next);
                for (const auto& original : local.at("points")) {
                    if (!original.value("completed", false)) continue;
                    auto& point = FindOrCreate(next, original);
                    point["completed"] = true;
                    point["localTouched"] = true;
                    point["pending"] = true;
                    point["revision"] = revision;
                }
                Commit(next);
                return Result(Snapshot(next));
            }
            if (type == "markerResolveConflict") {
                Json key = command;
                ValidatePoint(key, false);
                auto* point = Find(next, key);
                if (!point || point->value("revision", std::uint64_t{}) != command.at("expectedRevision").get<std::uint64_t>()) return Failure("revision-conflict");
                const bool completed = command.at("completed").get<bool>();
                (*point)["completed"] = completed;
                (*point)["remoteCompleted"] = completed;
                (*point)["pending"] = false;
                (*point)["revision"] = Advance(next);
                (*point)["acknowledgedRevision"] = (*point)["revision"];
                Commit(next);
                return Result(Snapshot(next));
            }
            if (type == "markerSetCompletion") {
                Json key = command;
                ValidatePoint(key);
                auto& point = FindOrCreate(next, key);
                const bool completed = command.at("completed").get<bool>();
                point["localTouched"] = true;
                if (point.value("completed", false) != completed || !point.contains("revision")) {
                    point["completed"] = completed;
                    point["revision"] = Advance(next);
                    point["pending"] = true;
                }
                Commit(next);
                return Result({{"profileId", profile}, {"revision", next.at("revision")}, {"point", point}});
            }
            if (type == "markerAcknowledgeSync") {
                Json key = command;
                ValidatePoint(key, false);
                auto* point = Find(next, key);
                if (!point) return Failure("unknown-point");
                const auto revision = command.at("revision").get<std::uint64_t>();
                if (revision > point->value("revision", std::uint64_t{})) return Failure("unknown-revision");
                if (revision >= point->value("acknowledgedRevision", std::uint64_t{})) {
                    (*point)["remoteCompleted"] = command.at("completed").get<bool>();
                    (*point)["acknowledgedRevision"] = revision;
                    if (revision == point->value("revision", std::uint64_t{}) &&
                        point->value("completed", false) == command.at("completed").get<bool>()) (*point)["pending"] = false;
                    Advance(next);
                }
                Commit(next);
                return Result(Snapshot(next));
            }
            if (type == "markerSetSyncState") {
                const int state = command.at("stateId").get<int>();
                auto& sync = SyncState(next, state);
                if (command.contains("initialized")) sync["initialized"] = command.at("initialized").get<bool>();
                if (command.contains("enabled")) sync["enabled"] = command.at("enabled").get<bool>();
                if (sync.value("enabled", false) && !sync.value("initialized", false)) return Failure("sync-not-initialized");
                Advance(next);
                Commit(next);
                return Result(Snapshot(next));
            }
            if (type == "markerApplyRemote" || type == "markerInitializeSync") {
                const int state = command.at("stateId").get<int>();
                const bool initialize = type == "markerInitializeSync";
                const auto mode = command.value("mode", "import");
                if (mode != "import" && mode != "merge" && mode != "upload") return Failure("invalid-sync-mode");
                std::set<std::string> remoteIds;
                for (const auto& id : command.value("remoteIds", Json::array())) remoteIds.insert(id.get<std::string>());
                // The adapter supplies known public identities; unknown cloud IDs
                // stay in remoteIds and are never turned into deletion operations.
                for (const auto& supplied : command.value("points", Json::array())) {
                    Json key = supplied;
                    ValidatePoint(key);
                    if (key.at("stateId").get<int>() != state) return Failure("state-mismatch");
                    const auto id = key.at("pointId").get<std::string>();
                    if (!command.contains("remoteIds") && supplied.value("completed", false)) remoteIds.insert(id);
                    if (remoteIds.contains(id) || Find(next, key)) FindOrCreate(next, key);
                }
                Json conflicts = Json::array();
                const auto revision = next.value("revision", std::uint64_t{}) + 1;
                for (auto& point : next["points"]) {
                    if (point.value("stateId", 0) != state) continue;
                    const bool remote = remoteIds.contains(point.at("pointId").get<std::string>());
                    const bool local = point.value("completed", false);
                    if (initialize) {
                        const bool desired = mode == "import" ? remote : (mode == "merge" ? local || remote :
                            (point.value("localTouched", false) ? local : remote));
                        point["completed"] = desired;
                        point["remoteCompleted"] = remote;
                        point["revision"] = revision;
                        point["pending"] = desired != remote;
                        point["acknowledgedRevision"] = desired == remote ? revision : 0;
                    } else if (point.value("pending", false)) {
                        if (!point.at("remoteCompleted").is_null() && point.at("remoteCompleted").get<bool>() != remote && local != remote)
                            conflicts.push_back(point);
                        // Preserve the old baseline while a local operation is pending.
                    } else if (point.value("completed", false) != remote || point.at("remoteCompleted").is_null() ||
                        point.at("remoteCompleted").get<bool>() != remote) {
                        point["completed"] = remote;
                        point["remoteCompleted"] = remote;
                        point["revision"] = revision;
                        point["acknowledgedRevision"] = revision;
                    }
                }
                auto& sync = SyncState(next, state);
                sync["remoteIds"] = remoteIds;
                if (initialize) { sync["initialized"] = true; sync["enabled"] = true; sync["initialMode"] = mode; }
                if (next != document) { next["revision"] = revision; Commit(next); }
                auto result = Snapshot(next);
                result["conflicts"] = std::move(conflicts);
                return Result(std::move(result));
            }
            return Failure("unknown-marker-command");
        } catch (const std::exception& error) { return Failure(error.what()); }
    }

    bool Completed(const std::string& scene, const std::string& name, const std::string& id) const {
        std::scoped_lock lock(mutex);
        const auto key = std::to_string(SceneState(scene)) + ":" + id;
        const auto& points = document.at("points");
        const auto found = points.find(key);
        if (found != points.end()) return found->value("nameId", "") == name && found->value("completed", false);
        return RemoteCompleted(document, SceneState(scene), id);
    }
    std::vector<std::string> CompletedIds(const std::string& scene, const std::string& name) const {
        std::scoped_lock lock(mutex);
        std::set<std::string> completed;
        const int state = SceneState(scene);
        for (const auto& id : RemoteIds(document, state)) completed.insert(id);
        for (const auto& point : document.at("points"))
            if (point.value("sceneName", "") == scene && point.value("nameId", "") == name) {
                const auto id = point.at("pointId").get<std::string>();
                if (point.value("completed", false)) completed.insert(id); else completed.erase(id);
            }
        return {completed.begin(), completed.end()};
    }
    std::string Profile() const { std::scoped_lock lock(mutex); return document.at("profileId").get<std::string>(); }

private:
    std::filesystem::path root;
    mutable std::mutex mutex;
    Json document;
    static Json Result(Json data) { return {{"accepted", true}, {"message", ""}, {"data", std::move(data)}}; }
    static Json Failure(const std::string& error) { return {{"accepted", false}, {"message", error}, {"data", Json::object()}}; }
    static void ValidateProfile(const std::string& id) {
        if (id.empty() || id.size() > 96 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '-' || c == '_'; })) throw std::invalid_argument("invalid-profile");
    }
    static void ValidatePoint(Json& key, bool requireNames = true) {
        const auto id = key.at("pointId").get<std::string>();
        if (id.empty() || id.size() > 128) throw std::invalid_argument("invalid-point-id");
        if (!key.contains("stateId")) key["stateId"] = SceneState(key.value("sceneName", ""));
        if (key.at("stateId").get<int>() <= 0) throw std::invalid_argument("invalid-state");
        if (requireNames && (key.value("sceneName", "").empty() || key.value("nameId", "").empty()))
            throw std::invalid_argument("unknown-point-identity");
    }
    static Json* Find(Json& doc, const Json& key) {
        const auto identity = std::to_string(key.at("stateId").get<int>()) + ":" + key.at("pointId").get<std::string>();
        const auto found = doc["points"].find(identity);
        return found == doc["points"].end() ? nullptr : &found.value();
    }
    static Json& FindOrCreate(Json& doc, const Json& key) {
        if (auto* point = Find(doc, key)) return *point;
        Json point = {{"sceneName", key.at("sceneName")}, {"nameId", key.at("nameId")},
            {"stateId", key.at("stateId")}, {"pointId", key.at("pointId")}, {"completed", false},
            {"remoteCompleted", nullptr}, {"pending", false}};
        for (const auto& sync : doc.at("syncStates")) {
            if (sync.at("stateId") == key.at("stateId") && sync.value("initialized", false)) {
                const auto& ids = sync.at("remoteIds");
                point["remoteCompleted"] = std::find(ids.begin(), ids.end(), key.at("pointId")) != ids.end();
                break;
            }
        }
        const auto identity = std::to_string(key.at("stateId").get<int>()) + ":" + key.at("pointId").get<std::string>();
        doc["points"][identity] = std::move(point);
        return doc["points"][identity];
    }
    static std::uint64_t Advance(Json& doc) {
        const auto value = doc.value("revision", std::uint64_t{}) + 1;
        doc["revision"] = value;
        return value;
    }
    static Json& SyncState(Json& doc, int state) {
        if (state <= 0) throw std::invalid_argument("invalid-state");
        for (auto& entry : doc["syncStates"]) if (entry.at("stateId") == state) return entry;
        doc["syncStates"].push_back({{"stateId", state}, {"initialized", false}, {"enabled", false}, {"remoteIds", Json::array()}});
        return doc["syncStates"].back();
    }
    static const Json& RemoteIds(const Json& doc, int state) {
        static const Json empty = Json::array();
        if (state <= 0) return empty;
        for (const auto& entry : doc.at("syncStates"))
            if (entry.at("stateId") == state && entry.value("initialized", false)) return entry.at("remoteIds");
        return empty;
    }
    static bool RemoteCompleted(const Json& doc, int state, const std::string& id) {
        const auto& ids = RemoteIds(doc, state);
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    }
    static Json Snapshot(const Json& doc, const Json& options = Json::object()) {
        Json points = Json::array();
        std::size_t total = 0;
        const auto offset = options.value("offset", std::size_t{});
        const auto limit = std::min<std::size_t>(500, options.value("limit", std::size_t{500}));
        for (const auto& point : doc.at("points")) {
            if (options.contains("stateId") && options.at("stateId") != point.at("stateId")) continue;
            if (options.contains("pointId") && options.at("pointId") != point.at("pointId")) continue;
            if (total >= offset && points.size() < limit) points.push_back(point);
            ++total;
        }
        return {{"profileId", doc.at("profileId")}, {"revision", doc.at("revision")}, {"points", points},
            {"total", total}, {"hasMore", offset + points.size() < total}, {"syncStates", doc.at("syncStates")}};
    }
    Json Load(const std::string& profile) const {
        ValidateProfile(profile);
        const auto path = root / "profiles" / (profile + ".json");
        if (std::filesystem::exists(path)) {
            std::ifstream input(path);
            auto result = Json::parse(input);
            if (result.value("schemaVersion", 0) != 2 || result.value("profileId", "") != profile ||
                !result.at("points").is_object() || !result.at("syncStates").is_array()) throw std::runtime_error("invalid-profile-document");
            return result;
        }
        Json result = {{"schemaVersion", 2}, {"profileId", profile}, {"revision", 0}, {"points", Json::object()}, {"syncStates", Json::array()}};
        const auto legacy = root / "account_1.json";
        if (profile == "local" && std::filesystem::exists(legacy)) {
            std::ifstream input(legacy);
            auto old = Json::parse(input);
            if (!old.is_object()) throw std::runtime_error("invalid-legacy-points");
            for (const auto& [scene, groups] : old.items()) {
                if (!groups.is_object() || SceneState(scene) == 0) continue;
                for (const auto& [name, points] : groups.items()) {
                    if (!points.is_array()) continue;
                    for (const auto& point : points) {
                        if (!point.contains("id") || !point.at("id").is_string()) continue;
                        auto& imported = FindOrCreate(result, {{"sceneName", scene}, {"nameId", name},
                            {"stateId", SceneState(scene)}, {"pointId", point.at("id")}});
                        imported["completed"] = true;
                        imported["revision"] = 0;
                        imported["localTouched"] = true;
                        // Legacy progress is local until the user chooses an initial policy.
                    }
                }
            }
        }
        return result;
    }
    void Commit(const Json& next) {
        WriteTextAtomically(root / "profiles" / (next.at("profileId").get<std::string>() + ".json"), next.dump(2));
        document = next;
    }
};
