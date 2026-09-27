#include "DrawItemBase.h"
#include <fstream>
#include "../../DLL_API.h"
#include "../../util.h"
#include "../../Coordinate/locationCalculator/RelativeCoordinates.h"
#include "../InteractiveInterface/Notification.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <shared_mutex>
#include <unordered_map>
#include <mutex>
#include "../../Runtime/SceneItemStore.h"
#include "../../Runtime/AtomicFile.h"
#include "../../Runtime/MarkerCompletionStore.h"
#include "../../Runtime/FarmCompletionStore.h"
#include "../../Runtime/RefreshableCategories.h"
#include "../../Runtime/RoutePlanningService.h"
#include "../../Runtime/MarkerGuideProtocol.h"
#include <functional>
#include "../../Runtime/StructuredLogger.h"
#include "../../Runtime/RouteGamepadBridge.h"
#include "../../Runtime/MapToolsBridge.h"
#include "../../Runtime/GamepadContext.h"
#include "../../Coordinate/KuroMapCoordinates.h"

using namespace std;
namespace fs = filesystem;

json DrawItemBase::itemsJsonData_World;
json DrawItemBase::itemsJsonData_Tethys;
json DrawItemBase::itemsJsonData_Fabricatorium;
json DrawItemBase::itemsJsonData_Avinoleum;
json DrawItemBase::itemsJsonData_Lahai;
json DrawItemBase::itemsJsonData_LowerVault;
json DrawItemBase::itemsJsonData_Darkplain;
json DrawItemBase::itemsJsonData_TimeRiftRuins;

vector<ItemTextureData> DrawItemBase::itemsTextureData;
unordered_map<string, size_t> DrawItemBase::itemTextureIndex;
static SceneItemStore<ItemsDatas> selectedItems;
thread DrawItemBase::thread_ReadSavedPointsJson;
std::atomic_bool DrawItemBase::savedPointsThreadStop = false;
std::filesystem::path DrawItemBase::savedJsonPath;

static std::unique_ptr<MarkerCompletionStore> markerStore;
// The daily-refresh ledger and the category table that decides what belongs in it. Both
// exist so the farming mode's completions can never enter the synchronized document.
static std::unique_ptr<FarmCompletionStore> farmStore;
static RefreshableCategories refreshableCategories;
// Every point id whose category is a daily-refresh one. Point ids are unique across all
// scenes and categories in the shipped data, so one flat set answers the question for a
// cloud identity that arrives with no category attached. A script asserts that uniqueness.
static std::unordered_set<std::string> refreshablePointIds;
static std::mutex markerEventMutex;
static std::function<void(const json&)> markerEventCallback;
static std::mutex markerGuideMutex;
static HWND markerGuideWindow = nullptr;
static json markerGuideRegistration = json::object();
static std::unordered_map<std::string, json> markerIdentities;
static std::mutex markerCandidatesMutex;
static json markerCandidates = json::array();
static std::string candidatesProfile, candidatesScene;
static std::uint64_t candidatesRevision = 0;
static std::optional<NearbySelection::Session> nearbySelection;
// What each point of the open candidate list already answered, so a lost response can
// be retried without writing twice while the next point is still a new request.
static std::map<std::string, json> nearbySelectionResults;
static std::atomic_uint64_t markerFilterRevision{1};
static std::mutex nearbyOperationMutex;
static std::uint64_t markerGuideRegistrationRevision = 0;
static bool LoadExternalKuroRuntimeJson(json& jsonData, const char* sceneName);

Coordinate KuroLocationToIdentifyCoordinate(const json& location) {
    return KuroPositionToGameCoordinates(location.at("x").get<double>(), location.at("y").get<double>());
}

// Moves completions of the daily-refresh categories (采集物 ∪ 敌人) out of the synchronized
// document and into the farming ledger. Their absence from the synchronized document is
// what keeps them out of the upload outbox and off the cloud comparison, so it has to hold
// for records that predate this feature as well as for the ones an import can still create.
std::size_t MoveRefreshableToFarmLedger() {
    if (!markerStore || !farmStore || refreshablePointIds.empty()) return 0;
    const auto extracted = markerStore->ExtractPoints([](const json& point) {
        return point.contains("pointId") && point.at("pointId").is_string() &&
            refreshablePointIds.contains(point.at("pointId").get<std::string>());
    });
    const auto& moved = extracted.at("points");
    if (moved.empty()) return 0;
    const auto absorbed = farmStore->Absorb(moved);
    StructuredLogger::Record("info", "farm", "moved-out-of-sync-ledger",
        "moved=" + std::to_string(moved.size()) + " completed=" + std::to_string(absorbed));
    DrawItemBase::PublishMarkerEvent({{"type", "markerFarmLedgerMoved"}, {"profileId", markerStore->Profile()},
        {"moved", static_cast<std::uint64_t>(moved.size())}});
    return moved.size();
}

json& DrawItemBase::GetSavedItemPoints() {
    static json j;
    return j;
}

void DrawItemBase::Initi() {
    DrawItemBase::LoadItemsjson();
    const auto directory = StructuredLogger::ApplicationDataDirectory() / "SavedPoints";
    fs::create_directories(directory);
    const auto legacy = fs::path(GetCurrentPath()) / "SavedPoints" / "account_1.json";
    if (!fs::exists(directory / "account_1.json") && fs::exists(legacy)) fs::copy_file(legacy, directory / "account_1.json");
    refreshableCategories = RefreshableCategories::Load(ResourceSnapshotContext::MapDataRoot());
    markerStore = std::make_unique<MarkerCompletionStore>(directory);
    farmStore = std::make_unique<FarmCompletionStore>(directory);
    markerIdentities.clear();
    refreshablePointIds.clear();
    for (const auto sceneId : Scene::sceneIds) {
        json* source = nullptr;
        if (!FindItemJsonData(sceneId, source) || !source || !source->is_array()) continue;
        const auto scene = Scene::SceneIdToName(sceneId);
        for (const auto& category : *source) {
            const auto categoryId = category.value("id", "");
            const bool refreshable = refreshableCategories.Contains(categoryId);
            for (const auto& location : category.value("location", json::array())) {
                if (!location.contains("id") || !location.at("id").is_string()) continue;
                const auto id = location.at("id").get<std::string>();
                const int state = location.value("stateId", MarkerCompletionStore::SceneState(scene));
                markerIdentities[std::to_string(state) + ":" + id] = {{"sceneName", scene},
                    {"nameId", categoryId}, {"stateId", state}, {"pointId", id}};
                // Point ids are unique across all scenes and categories, so one flat set can
                // answer for a cloud identity that arrives with no category attached.
                if (refreshable) refreshablePointIds.insert(id);
            }
        }
    }
    if (refreshableCategories.Empty())
        StructuredLogger::Record("warn", "farm", "refresh-categories-unavailable", refreshableCategories.Diagnostic());
    else
        StructuredLogger::Record("info", "farm", "refresh-categories",
            "categories=" + std::to_string(refreshableCategories.Size()) + " points=" + std::to_string(refreshablePointIds.size()) +
            (refreshableCategories.UnreadableFiles() ? " unreadable=" + std::to_string(refreshableCategories.UnreadableFiles()) : ""));
    MoveRefreshableToFarmLedger();
    RoutePlanningService::Initialize();
}

void DrawItemBase::Shutdown() {
    RoutePlanningService::Shutdown();
    SetMarkerEventCallback({});
    SetGuideWindow(nullptr);
    markerStore.reset();
    farmStore.reset();
    refreshablePointIds.clear();
    refreshableCategories = RefreshableCategories{};
}

bool LoadJson(json& JsonData, const wchar_t* resourceName) {
    HRSRC hResInfo = FindResource(g_hDllInstance, resourceName, L"JSON");
    if (!hResInfo) {
        std::cout << "无法找到资源！" << std::endl;
        return false;
    }

    HGLOBAL hResData = LoadResource(g_hDllInstance, hResInfo);
    if (!hResData) {
        std::cout << "无法加载资源！" << std::endl;
        return false;
    }

    LPVOID pResData = LockResource(hResData);
    if (!pResData) {
        std::cout << "无法锁定资源！" << std::endl;
        return false;
    }

    DWORD resSize = SizeofResource(g_hDllInstance, hResInfo);

    try {
        std::string jsonStr(static_cast<const char*>(pResData), resSize);
        JsonData = json::parse(jsonStr);
        return JsonData.is_array();
    }
    catch (const json::parse_error& e) {
        std::cout << "JSON parsing error:" << e.what() << std::endl;
        return false;
    }
}

void DrawItemBase::LoadItemsjson() {
    const wchar_t* embedded[] = {L"ITEMSJSON_World", L"ITEMSJSON_Tethys", L"ITEMSJSON_Fabricatorium",
        L"ITEMSJSON_Avinoleum", L"ITEMSJSON_Lahai"};
    for (const auto sceneId : Scene::sceneIds) {
        json* data = nullptr;
        if (!FindItemJsonData(sceneId, data) || !data) throw std::runtime_error("unknown runtime scene");
        *data = json::array();
        const auto name = Scene::SceneIdToName(sceneId);
        if (!Scene::IsRuntimeApproved(sceneId)) continue;
        const auto path = ResourceSnapshotContext::MapDataRoot() / "runtime" / ("itemsData_" + name + ".json");
        if (fs::exists(path)) {
            if (!LoadExternalKuroRuntimeJson(*data, name.c_str())) throw std::runtime_error("invalid external points for " + name);
        } else if (ResourceSnapshotContext::Strict() || sceneId > 5 || !LoadJson(*data, embedded[sceneId - 1])) {
            throw std::runtime_error("required scene points missing: " + name);
        }
    }
}

static bool LoadExternalKuroRuntimeJson(json& jsonData, const char* sceneName) {
    try {
        const int sceneId = Scene::SceneNameToId(sceneName);
        if (!Scene::IsRuntimeApproved(sceneId)) {
            cerr << "Kuro map scene is not release-approved: " << sceneName << endl;
            return false;
        }
        const fs::path path = ResourceSnapshotContext::MapDataRoot() / "runtime" /
            ("itemsData_" + string(sceneName) + ".json");
        ifstream input(path);
        if (!input) return false;
        input >> jsonData;
        return jsonData.is_array();
    }
    catch (const exception& exception) {
        cerr << "Unable to load external Kuro runtime JSON: " << exception.what() << endl;
        return false;
    }
}

bool DrawItemBase::IsValidItemNameId(string itemNameId) {
    for (const int scene : Scene::sceneIds) {
        const auto snapshot = selectedItems.Read(scene);
        for (const auto& group : *snapshot) if (group.nameId == itemNameId) return true;
    }
    return false;
}

std::shared_ptr<const std::vector<ItemsDatas>> DrawItemBase::GetSceneItemsSnapshot(int sceneId) {
    return selectedItems.Read(sceneId);
}

string DrawItemBase::GetExternalIconPath(const string& itemNameId) {
    static once_flag manifestLoadOnce;
    static unordered_map<string, string> iconPaths;

    call_once(manifestLoadOnce, []() {
        try {
            // Icon paths inside the manifest are relative to the manifest itself, so moving
            // icon-manifest.json and icons/ into their own package needs only this root change.
            const fs::path manifestPath = ResourceSnapshotContext::MapIconRoot() / "icon-manifest.json";
            ifstream file(manifestPath);
            if (!file) {
                return;
            }

            json manifest;
            file >> manifest;
            if (!manifest.contains("icons") || !manifest["icons"].is_object()) {
                return;
            }

            for (const auto& [id, relativePath] : manifest["icons"].items()) {
                if (!relativePath.is_string()) {
                    continue;
                }
                const fs::path candidate = fs::path(relativePath.get<string>());
                if (candidate.is_absolute() || candidate.empty() || candidate.has_root_name() ||
                    std::any_of(candidate.begin(), candidate.end(), [](const auto& component) { return component == ".."; })) {
                    continue;
                }
                const fs::path fullPath = manifestPath.parent_path() / candidate;
                if (fs::exists(fullPath) && fs::is_regular_file(fullPath)) {
                    iconPaths.emplace(id, fullPath.string());
                }
            }
        }
        catch (const exception& e) {
            cerr << "Unable to load Kuro map icon manifest: " << e.what() << endl;
        }
    });

    const auto icon = iconPaths.find(itemNameId);
    return icon == iconPaths.end() ? string() : icon->second;
}

bool DrawItemBase::FindItemJsonData(int sceneId, json*& data) {
    switch (sceneId) {
    case 1: data = &itemsJsonData_World; break;
    case 2: data = &itemsJsonData_Tethys; break;
    case 3: data = &itemsJsonData_Fabricatorium; break;
    case 4: data = &itemsJsonData_Avinoleum; break;
    case 5: data = &itemsJsonData_Lahai; break;
    case 6: data = &itemsJsonData_LowerVault; break;
    case 7: data = &itemsJsonData_Darkplain; break;
    case 8: data = &itemsJsonData_TimeRiftRuins; break;
    default: data = nullptr; return false;
    }
    return true;
}

void DrawItemBase::AddItemDataFromJson(string itemId) {
    try {
        json* itemsJsonDataPtr = nullptr;


        for (const auto& sceneId : Scene::sceneIds) {
            vector<ItemDatas> itemsDatas;
           
            if (!FindItemJsonData(sceneId, itemsJsonDataPtr))
                return;

            for (const auto& [item_id, item_info] : (*itemsJsonDataPtr).items()) {
                    string nameId = item_info["id"].get<string>();
                if (nameId == itemId) {
                    for (const auto& location : item_info["location"]) {
                        const Coordinate identifyCoordinate = KuroLocationToIdentifyCoordinate(location);
                        Coordinate itemMapROC = RelativeCoordinates::IdentifyCoordToROC(identifyCoordinate, sceneId);
   
                        string s = location["id"].get<string>();
                        ItemDatas tempItemDatas = { s ,nameId,Coordinate(0,0),itemMapROC ,false };
                        tempItemDatas.layer.stateId = location.value("stateId", Scene::Find(sceneId)->kuroStateId);
                        if (tempItemDatas.layer.stateId <= 0) tempItemDatas.layer.stateId = Scene::Find(sceneId)->kuroStateId;
                        tempItemDatas.layer.countryId = location.value("countryId", 0);
                        const auto metadata = [&](const char* key) {
                            if (!location.contains(key) || location.at(key).is_null()) return std::string{};
                            return location.at(key).is_string() ? location.at(key).get<std::string>() : location.at(key).dump();
                        };
                        tempItemDatas.layer.floorId = metadata("floorId");
                        tempItemDatas.layer.level = metadata("level");
                        itemsDatas.push_back(tempItemDatas);
                    }
                   std::scoped_lock filterLock(nearbyOperationMutex);
                   selectedItems.Add(sceneId, ItemsDatas(nameId, std::move(itemsDatas)));
                   ++markerFilterRevision;
                }
            }

        }
    }
    catch (const exception& e) {
        cerr << "Exception in AddItemDataFromJson: " << e.what() << endl;
        return;
    }
}

void DrawItemBase::ClearItemData(string itemId) {
    std::scoped_lock filterLock(nearbyOperationMutex);
    selectedItems.Remove(itemId);
    ++markerFilterRevision;
}

void DrawItemBase::RenderPointCircle(ImTextureID texture, ImVec2 position,float radius,float transparency, ImColor circleColor) {
    auto draw = ImGui::GetBackgroundDrawList();
    draw->AddCircleFilled(position, radius, ImColor(0.23f, 0.26f, 0.32f, transparency));

    draw->AddImageRounded(texture, ImVec2(position.x - radius, position.y - radius), ImVec2(position.x + radius, position.y + radius), ImVec2(0, 0), ImVec2(1, 1), ImColor(1.0f, 1.0f, 1.0f, transparency), radius);

    draw->AddCircle(position, radius, circleColor);
}

void DrawItemBase::SaveItemPoint(string scene, ItemDatas itemDatas) {
    const auto result = HandleMarkerCommand({{"type", "markerSetCompletion"}, {"sceneName", scene},
        {"profileId", MarkerProfile()},
        {"nameId", itemDatas.nameId}, {"pointId", itemDatas.itemId},
        {"stateId", itemDatas.layer.stateId > 0 ? itemDatas.layer.stateId : MarkerCompletionStore::SceneState(scene)}, {"completed", true}});
    if (!result.value("accepted", false)) Notification::AddError(NotificationDatas("保存标记失败：" + result.value("message", ""), 5));
}

void DrawItemBase::RemoveSavedItemPoint(string scene, ItemDatas itemDatas) {
    const auto result = HandleMarkerCommand({{"type", "markerSetCompletion"}, {"sceneName", scene},
        {"profileId", MarkerProfile()},
        {"nameId", itemDatas.nameId}, {"pointId", itemDatas.itemId},
        {"stateId", itemDatas.layer.stateId > 0 ? itemDatas.layer.stateId : MarkerCompletionStore::SceneState(scene)}, {"completed", false}});
    if (!result.value("accepted", false)) Notification::AddError(NotificationDatas("保存标记失败：" + result.value("message", ""), 5));
}

vector<string> DrawItemBase::GetFilteredPoints(string scene, string nameId) {
    if (!markerStore) return vector<string>{};
    // The daily-refresh categories are answered from their own ledger. Going through the
    // synchronized one would show a completion the 04:00 reset already cleared.
    if (IsRefreshablePoint(nameId))
        return farmStore ? farmStore->CompletedIds(MarkerCompletionStore::SceneState(scene)) : vector<string>{};
    return markerStore->CompletedIds(scene, nameId);
}

bool DrawItemBase::IsPointCompleted(const string& scene, const ItemDatas& item) {
    if (!markerStore) return false;
    const int state = item.layer.stateId > 0 ? item.layer.stateId : MarkerCompletionStore::SceneState(scene);
    // A daily-refresh point reads its own ledger and nothing else: a leftover record in the
    // synchronized document, or a cloud completion the baseline still carries, must never
    // make the daily reset look like it did not happen.
    if (IsRefreshablePoint(item.nameId) || IsRefreshablePointId(item.itemId))
        return farmStore && farmStore->Completed(state, item.itemId);
    return markerStore->Completed(scene, item.nameId, item.itemId);
}

std::string DrawItemBase::MarkerProfile() { return markerStore ? markerStore->Profile() : "local"; }
std::uint64_t DrawItemBase::MarkerFilterRevision() { return markerFilterRevision.load(); }

// One nearby point a key press may act on, re-resolved under the operation lock.
struct NearbyTarget {
    bool accepted = false;
    std::string reason;
    NearbySelection::Candidate candidate;
    std::string profileId, sceneName;
};

// A repeated frame is not permission to repeat a choice: the arrow moving on,
// another icon starting to overlap the chosen one, or a changed filter, account,
// scene or expired position all refuse instead of inheriting the original point.
static NearbyTarget ResolveNearbyTarget(const NearbySelection::Observation& initial, NearbySelection::Intent intent) {
    std::scoped_lock operationLock(nearbyOperationMutex);
    const auto reject = [](const std::string& reason) { return NearbyTarget{false, reason}; };
    if (initial.profileId != DrawItemBase::MarkerProfile()) return reject("nearby-context-changed");
    auto current = GamepadContextSnapshot::Shared().ReadNearby(initial.profileId);
    // Refresh durable completion under the same operation lock used for chooser and
    // filter changes, so a point completed meanwhile is never written twice.
    std::erase_if(current.candidates, [&](const auto& item) { return DrawItemBase::IsPointCompleted(current.sceneName, item.item); });
    const auto failure = NearbySelection::ValidateSingleSelection(initial, current, DrawItemBase::MarkerFilterRevision(), intent);
    if (!failure.empty()) return reject(failure);
    const auto game = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(current.gameHwnd));
    DWORD pid = 0;
    if (!DrawItemBase::IsMarkerGameFocused(game) || !IsWindowVisible(game) || IsIconic(game) ||
        !GetWindowThreadProcessId(game, &pid) || pid != current.gameProcessId) return reject("nearby-game-changed");
    const auto group = NearbySelection::Resolve(current.candidates, intent, NearbySelection::OverlapDiameter(current));
    if (group.size() != 1) return reject("nearby-single-selection-changed");
    return {true, "", group.front(), current.profileId, current.sceneName};
}

json DrawItemBase::CompleteNearbySingle(const NearbySelection::Observation& initial) {
    const auto target = ResolveNearbyTarget(initial, NearbySelection::Intent::Complete);
    if (!target.accepted) {
        StructuredLogger::Record("info", "gamepad", "nearby-single-rejected", target.reason);
        return json{{"accepted", false}, {"message", target.reason}, {"data", json::object()}};
    }
    const auto& item = target.candidate.item;
    return HandleMarkerCommand({{"type", "markerSetCompletion"}, {"profileId", target.profileId},
        {"sceneName", target.sceneName}, {"nameId", item.nameId}, {"stateId", item.layer.stateId},
        {"pointId", item.itemId}, {"completed", true}});
}

// A guide writes nothing, so the resolved identity is returned to its caller in the
// same flat shape the candidate page uses; an empty object means the point or the
// position changed and the player has to press the key again.
json DrawItemBase::ResolveNearbyGuide(const NearbySelection::Observation& initial) {
    const auto target = ResolveNearbyTarget(initial, NearbySelection::Intent::Guide);
    if (!target.accepted) {
        StructuredLogger::Record("info", "gamepad", "nearby-single-rejected", target.reason);
        return json::object();
    }
    const auto& item = target.candidate.item;
    return {{"profileId", target.profileId}, {"sceneName", target.sceneName}, {"intent", "guide"},
        {"selection", {{"profileId", target.profileId}, {"sceneName", target.sceneName}, {"nameId", item.nameId},
            {"pointId", item.itemId}, {"stateId", item.layer.stateId}, {"countryId", item.layer.countryId},
            {"floorId", item.layer.floorId}, {"level", item.layer.level}, {"completed", false}}}};
}

static json HandleNearbyCommand(const json& command) {
    const auto type = command.value("type", "");
    std::scoped_lock operationLock(nearbyOperationMutex);
    std::scoped_lock candidatesLock(markerCandidatesMutex);
    const auto reject = [&](const std::string& reason) {
        StructuredLogger::Record("info", "gamepad", "nearby-submit-rejected", type + " reason=" + reason);
        return json{{"accepted", false}, {"message", reason}, {"data", json::object()}};
    };
    if (!nearbySelection) return reject("selection-expired");
    auto& selection = *nearbySelection;
    const auto profile = command.value("profileId", "");
    if (command.value("selectionRevision", std::uint64_t{}) != selection.revision ||
        profile != selection.source.profileId || profile != DrawItemBase::MarkerProfile()) return reject("selection-expired");
    const auto requestedWindow = MarkerGuideProtocol::Integer(command.at("chooserHwnd"), false);
    const auto requestedGeneration = MarkerGuideProtocol::Integer(command.at("chooserGeneration"), false);
    const auto window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(requestedWindow));
    std::scoped_lock guideLock(markerGuideMutex);
    if (window != markerGuideWindow || !IsWindow(window)) return reject("nearby-window-changed");
    const auto current = GamepadContextSnapshot::Shared().ReadNearby(profile);
    const auto game = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(selection.source.gameHwnd));
    DWORD pid = 0;
    if (!IsWindow(game) || !IsWindowVisible(game) || IsIconic(game) || !GetWindowThreadProcessId(game, &pid) ||
        pid != selection.source.gameProcessId || current.session != selection.source.session ||
        current.gameHwnd != selection.source.gameHwnd || current.gameProcessId != pid)
        return reject("nearby-game-changed");
    if (type == "markerBindNearbyCandidates") {
        if (selection.chooserHwnd && (selection.chooserHwnd != requestedWindow ||
            selection.chooserGeneration != requestedGeneration || selection.registrationRevision != markerGuideRegistrationRevision))
            return reject("nearby-window-changed");
        selection.chooserHwnd = requestedWindow; selection.chooserGeneration = requestedGeneration;
        selection.registrationRevision = markerGuideRegistrationRevision;
        StructuredLogger::Record("info", "gamepad", "nearby-chooser-bound", "revision=" + std::to_string(selection.revision));
        return {{"accepted", true}, {"data", json::object()}};
    }
    if (selection.chooserHwnd != requestedWindow || selection.chooserGeneration != requestedGeneration ||
        selection.registrationRevision != markerGuideRegistrationRevision || GetForegroundWindow() != window ||
        !IsWindowVisible(window) || IsIconic(window)) return reject("nearby-window-changed");
    const auto intent = type == "markerCompleteNearbyAll" ? NearbySelection::Intent::Complete :
        type == "markerCompleteNearbyCandidate" ? NearbySelection::Intent::Complete : NearbySelection::Intent::Guide;
    if (selection.intent != intent) return reject("nearby-intent-mismatch");
    if (type == "markerCompleteNearbyAll") {
        // One button for the whole list: every point is validated exactly like a single
        // submit, and one that left the group, went stale or was completed meanwhile is
        // skipped instead of written.
        json completed = json::array(), skipped = json::array(), failed = json::array();
        for (const auto& candidate : selection.source.candidates) {
            if (!NearbySelection::Includes(candidate, NearbySelection::Intent::Complete)) continue;
            const auto candidateKey = NearbySelection::Key(candidate.item);
            if (nearbySelectionResults.contains(candidateKey)) { completed.push_back(candidateKey); continue; }
            const auto candidateFailure = selection.Validate(current, DrawItemBase::MarkerFilterRevision(),
                selection.revision, profile, current.sceneName, candidateKey);
            if (!candidateFailure.empty() || DrawItemBase::IsPointCompleted(current.sceneName, candidate.item)) {
                skipped.push_back(candidateKey); continue;
            }
            json candidatePoint{{"type", "markerSetCompletion"}, {"profileId", profile}, {"sceneName", current.sceneName},
                {"nameId", candidate.item.nameId}, {"stateId", candidate.item.layer.stateId},
                {"pointId", candidate.item.itemId}, {"completed", true}};
            const auto candidateResult = DrawItemBase::HandleMarkerCommand(candidatePoint);
            if (!candidateResult.value("accepted", false)) { failed.push_back(candidateKey); continue; }
            completed.push_back(candidateKey);
            nearbySelectionResults[candidateKey] = candidateResult;
        }
        StructuredLogger::Record("info", "gamepad", "nearby-collect-all", "revision=" + std::to_string(selection.revision) +
            " completed=" + std::to_string(completed.size()) + " skipped=" + std::to_string(skipped.size()) +
            " failed=" + std::to_string(failed.size()));
        return {{"accepted", true}, {"message", ""}, {"data", {{"completed", completed}, {"skipped", skipped}, {"failed", failed}}}};
    }
    const auto key = std::to_string(command.at("stateId").get<int>()) + ":" + command.at("pointId").get<std::string>();
    // A response may be lost after persistence. Repeating the exact same request returns
    // its saved response; a different point is a new request, because the list stays open
    // so the player can complete one nearby point after another.
    if (const auto saved = nearbySelectionResults.find(key); saved != nearbySelectionResults.end()) return saved->second;
    const auto failure = selection.Validate(current, DrawItemBase::MarkerFilterRevision(),
        command.at("selectionRevision").get<std::uint64_t>(), profile, command.value("sceneName", ""), key);
    if (!failure.empty()) return reject(failure);
    const auto candidate = std::find_if(current.candidates.begin(), current.candidates.end(),
        [&](const auto& item) { return NearbySelection::Key(item.item) == key; });
    if (candidate == current.candidates.end() || DrawItemBase::IsPointCompleted(current.sceneName, candidate->item))
        return reject("nearby-point-already-completed");
    if (GetForegroundWindow() != window) return reject("nearby-window-changed");
    const auto& item = candidate->item;
    json point{{"profileId", profile}, {"sceneName", current.sceneName}, {"nameId", item.nameId},
        {"pointId", item.itemId}, {"stateId", item.layer.stateId}, {"countryId", item.layer.countryId},
        {"floorId", item.layer.floorId}, {"level", item.layer.level}, {"completed", false}};
    json result;
    if (intent == NearbySelection::Intent::Guide) result = {{"accepted", true}, {"data", {{"selection", point}}}};
    else {
        point["type"] = "markerSetCompletion"; point["completed"] = true;
        result = DrawItemBase::HandleMarkerCommand(point);
    }
    if (intent == NearbySelection::Intent::Complete && result.value("accepted", false))
        nearbySelectionResults[key] = result;
    StructuredLogger::Record("info", "gamepad", "nearby-submit-result", type + " revision=" +
        std::to_string(selection.revision) + " point=" + key + " accepted=" + std::to_string(result.value("accepted", false)));
    return result;
}

json DrawItemBase::HandleMarkerCommand(const json& command) {
    if (!markerStore) return {{"accepted", false}, {"message", "marker-store-unavailable"}, {"data", json::object()}};
    const auto nearbyType = command.value("type", "");
    if (nearbyType == "markerBindNearbyCandidates" || nearbyType == "markerResolveNearbyCandidate" ||
        nearbyType == "markerCompleteNearbyCandidate" || nearbyType == "markerCompleteNearbyAll") return HandleNearbyCommand(command);
    auto normalized = command;
    const auto type = command.value("type", "");
    // Validate before saving so a malformed correlation field cannot result in
    // a successful write followed by a failed response.
    const auto guideContext = type == "markerSetCompletion" ? MarkerGuideProtocol::CompletionContext(command) : json::object();
    if (type == "markerGetCandidates") {
        std::scoped_lock lock(markerCandidatesMutex);
        if (command.value("profileId", "") != candidatesProfile || candidatesProfile != MarkerProfile() ||
            command.value("selectionRevision", std::uint64_t{}) != candidatesRevision)
            return {{"accepted", false}, {"message", "selection-expired"}, {"data", json::object()}};
        const auto offset = command.value("offset", std::size_t{});
        const auto limit = std::min<std::size_t>(100, command.value("limit", std::size_t{100}));
        json page = json::array();
        for (auto index = offset; index < markerCandidates.size() && index < offset + limit; ++index) page.push_back(markerCandidates[index]);
        return {{"accepted", true}, {"message", ""}, {"data", {{"candidates", page}, {"profileId", candidatesProfile},
            {"selectionRevision", candidatesRevision}, {"intent", nearbySelection && nearbySelection->intent == NearbySelection::Intent::Guide ? "guide" : "complete"},
            {"total", markerCandidates.size()}, {"hasMore", offset + page.size() < markerCandidates.size()}}}};
    }
    if (type == "markerSetCompletion" && command.contains("stateId") && command.contains("pointId")) {
        const auto key = std::to_string(command.at("stateId").get<int>()) + ":" + command.at("pointId").get<std::string>();
        const auto identity = markerIdentities.find(key);
        if (identity == markerIdentities.end()) return {{"accepted", false}, {"message", "unknown-public-point"}, {"data", json::object()}};
        for (const auto& [name, value] : identity->second.items()) normalized[name] = value;
        // The daily-refresh categories are kept out of the synchronized document entirely:
        // a completion for one of them is written to the farming ledger instead, which is
        // also what keeps it out of the upload outbox and off the cloud comparison. Reads
        // are redirected the same way (IsPointCompleted), so a record left in the old
        // document by an earlier version can never resurface as a completion either.
        if (IsRefreshablePointId(normalized.at("pointId").get<std::string>()))
            return HandleFarmSetCompletion(normalized);
    }
    if (type == "markerFarmComplete") return HandleFarmBatchCompletion(normalized);
    // An empty list means the caller supplied no identities (the desktop never
    // has the catalog), so it must be treated the same as an absent field.
    const bool suppliedIdentities = normalized.contains("points") && normalized.at("points").is_array() && !normalized.at("points").empty();
    if ((type == "markerApplyRemote" || type == "markerInitializeSync" || type == "markerPreviewSync") && !suppliedIdentities) {
        // A missing state selects every region: the cloud set is account-wide and
        // the store groups it by the local catalog each identity belongs to.
        const int state = command.value("stateId", 0);
        normalized["points"] = json::array();
        for (const auto& [key, identity] : markerIdentities)
            if (state <= 0 || identity.at("stateId") == state) normalized["points"].push_back(identity);
    }
    // A cloud completion for a daily-refresh point must not be applied: it would resurrect
    // the point the 04:00 reset just cleared, and its identity must not be reported as
    // "unmapped" either. These categories take no part in synchronization in either
    // direction, so the cloud set enters the store already without them.
    if ((type == "markerPreviewSync" || type == "markerApplyRemote" || type == "markerInitializeSync") &&
        !refreshablePointIds.empty() && normalized.contains("remoteIds") && normalized.at("remoteIds").is_array()) {
        json kept = json::array();
        for (const auto& id : normalized.at("remoteIds"))
            if (!id.is_string() || !refreshablePointIds.contains(id.get<std::string>())) kept.push_back(id);
        normalized["remoteIds"] = std::move(kept);
    }
    auto result = markerStore->Execute(normalized);
    if (result.value("accepted", false)) {
        if (type == "markerSetCompletion" || type == "markerApplyRemote" || type == "markerInitializeSync" ||
            type == "markerResolveConflict" || type == "markerCopyLocalProgress" || type == "markerImportLegacyProgress") {
            json event = {{"type", "markerCompletionChanged"}, {"profileId", markerStore->Profile()},
                {"source", type == "markerSetCompletion" || type == "markerCopyLocalProgress" ||
                    type == "markerImportLegacyProgress" ? "local" : "cloud"},
                {"revision", result.at("data").value("revision", std::uint64_t{})}};
            if (result.at("data").contains("point")) event["point"] = result.at("data").at("point");
            for (const auto& [name, value] : guideContext.items()) event[name] = value;
            PublishMarkerEvent(std::move(event));
        } else if (type == "markerSelectProfile") {
            // The farming ledger follows the record book the map is showing.
            farmStore->SelectProfile(markerStore->Profile());
            MoveRefreshableToFarmLedger();
            ClearMarkerCandidates(true);
            PublishMarkerEvent({{"type", "markerSelectionCleared"}});
            PublishMarkerEvent({{"type", "markerProfileChanged"}, {"profileId", markerStore->Profile()}});
        }
        // An import can still create daily-refresh records (the pre-rewrite file and the
        // explicit copy carry whatever the old record held), so the move has to run again
        // after every command that can add them.
        if (type == "markerApplyRemote" || type == "markerInitializeSync" ||
            type == "markerCopyLocalProgress" || type == "markerImportLegacyProgress") MoveRefreshableToFarmLedger();
    }
    return result;
}

bool DrawItemBase::IsRefreshablePoint(const std::string& nameId) {
    return !nameId.empty() && refreshableCategories.Contains(nameId);
}

bool DrawItemBase::IsRefreshablePointId(const std::string& pointId) {
    return !pointId.empty() && refreshablePointIds.contains(pointId);
}

// A completion of a daily-refresh point, written to its own ledger. The request is answered
// in the same shape MarkerCompletionStore uses so every existing caller keeps working.
json DrawItemBase::HandleFarmSetCompletion(const json& command) {
    if (!farmStore) return {{"accepted", false}, {"message", "farm-store-unavailable"}, {"data", json::object()}};
    const int state = command.value("stateId", 0);
    const auto pointId = command.at("pointId").get<std::string>();
    const auto profile = MarkerProfile();
    const auto requested = command.value("profileId", profile);
    if (!requested.empty() && requested != profile)
        return {{"accepted", false}, {"message", "profile-mismatch"}, {"data", json::object()}};
    const bool completed = command.value("completed", false);
    farmStore->Set(state, pointId, completed);
    json point = {{"sceneName", command.value("sceneName", "")}, {"nameId", command.value("nameId", "")},
        {"stateId", state}, {"pointId", pointId}, {"completed", completed},
        {"remoteCompleted", nullptr}, {"pending", false}, {"dailyRefresh", true}};
    PublishMarkerEvent({{"type", "markerFarmCompletionChanged"}, {"profileId", profile}, {"source", "local"},
        {"count", std::size_t{1}}, {"point", point}});
    return {{"accepted", true}, {"message", ""}, {"data", {{"point", point}, {"points", json::array({point})}}}};
}

// One write for a batch of targets the farming mode reached at once. Kept separate from the
// per-point command so a cluster of three monsters produces one revision and one notice
// instead of three.
json DrawItemBase::HandleFarmBatchCompletion(const json& command) {
    if (!farmStore) return {{"accepted", false}, {"message", "farm-store-unavailable"}, {"data", json::object()}};
    const auto profile = MarkerProfile();
    const auto requested = command.value("profileId", profile);
    if (!requested.empty() && requested != profile)
        return {{"accepted", false}, {"message", "profile-mismatch"}, {"data", json::object()}};
    std::vector<FarmCompletionStore::Entry> entries;
    json keys = json::array();
    for (const auto& supplied : command.value("points", json::array())) {
        const int state = supplied.value("stateId", 0);
        const auto pointId = supplied.value("pointId", std::string{});
        if (state <= 0 || pointId.empty() || !IsRefreshablePointId(pointId)) continue;
        // A caller that does state its category has to agree with the table.
        const auto nameId = supplied.value("nameId", std::string{});
        if (!nameId.empty() && !refreshableCategories.Contains(nameId)) continue;
        entries.push_back({state, pointId, true});
        keys.push_back(std::to_string(state) + ":" + pointId);
    }
    if (entries.empty()) return {{"accepted", false}, {"message", "no-refreshable-target"}, {"data", json::object()}};
    const auto changed = farmStore->SetMany(entries);
    if (changed) {
        // The one write the player did not ask for, so it is also the one that has to leave a
        // trace: the ledger file says which points, this says when and against which game day.
        StructuredLogger::Record("info", "farm", "auto-complete",
            "count=" + std::to_string(changed) + " epoch=" + std::to_string(farmStore->Epoch()) + " points=" + keys.dump());
        PublishMarkerEvent({{"type", "markerFarmCompletionChanged"}, {"profileId", profile}, {"source", "local"},
            {"count", changed}, {"keys", keys}});
    }
    return {{"accepted", true}, {"message", ""}, {"data", {{"completed", keys}, {"changed", changed}}}};
}

// The 04:00 reset happens on a read, so without this the day boundary would pass unnoticed:
// nothing else in the log would explain why the monsters the player marked are on the map
// again. Returns true exactly once per boundary, and only then is it worth telling the route
// service — its own `completed` set is a snapshot that would otherwise stay stale until the
// next marker event, leaving the route pointing at yesterday's "next target".
bool DrawItemBase::ReportFarmLedgerExpiry() {
    if (!farmStore || !farmStore->TakeExpired()) return false;
    StructuredLogger::Record("info", "farm", "ledger-expired",
        "epoch=" + std::to_string(farmStore->Epoch()) + " (the game refilled every 采集物 and 敌人; this ledger was cleared)");
    return true;
}


void DrawItemBase::SetMarkerEventCallback(std::function<void(const json&)> callback) {
    std::scoped_lock lock(markerEventMutex);
    markerEventCallback = std::move(callback);
}

void DrawItemBase::PublishMarkerEvent(json event) {
    const auto type = event.value("type", "");
    // A farming completion changes which route targets are left just like an ordinary one,
    // and the move that takes a whole class of records out of the synchronized document can
    // change the visible state too, so both have to refresh the route.
    if (type == "markerCompletionChanged" || type == "markerFarmCompletionChanged" ||
        type == "markerFarmLedgerMoved" || type == "markerProfileChanged") RoutePlanningService::OnMarkerChanged();
    std::function<void(const json&)> callback;
    { std::scoped_lock lock(markerEventMutex); callback = markerEventCallback; }
    if (callback) callback(event);
}

void DrawItemBase::PublishMarkerCandidates(const std::string& profileId, const std::string& sceneName, json candidates,
    bool gamepad, std::uint64_t gameHwnd) {
    json event;
    {
        std::scoped_lock lock(markerCandidatesMutex);
        nearbySelection.reset(); nearbySelectionResults.clear();
        markerCandidates = std::move(candidates);
        candidatesProfile = profileId; candidatesScene = sceneName; ++candidatesRevision;
        json page = json::array();
        for (std::size_t index = 0; index < std::min<std::size_t>(100, markerCandidates.size()); ++index) page.push_back(markerCandidates[index]);
        event = {{"type", "markerCandidates"}, {"intent", "complete"}, {"profileId", profileId},
            {"candidates", page}, {"selectionRevision", candidatesRevision}, {"total", markerCandidates.size()}, {"hasMore", page.size() < markerCandidates.size()}};
        if (gamepad) { event["gamepad"] = true; event["gameHwnd"] = gameHwnd; }
    }
    PublishMarkerEvent(std::move(event));
}
json DrawItemBase::PublishNearbyCandidates(NearbySelection::Observation observation, NearbySelection::Intent intent, bool gamepad, bool publish) {
    json event;
    {
        std::scoped_lock lock(markerCandidatesMutex);
        markerCandidates = json::array();
        POINT cursor{}; GetCursorPos(&cursor);
        for (const auto& candidate : observation.candidates) {
            if (!NearbySelection::Includes(candidate, intent)) continue;
            const auto& item = candidate.item;
            markerCandidates.push_back({{"profileId", observation.profileId}, {"sceneName", observation.sceneName},
                {"nameId", item.nameId}, {"pointId", item.itemId}, {"stateId", item.layer.stateId},
                {"countryId", item.layer.countryId}, {"floorId", item.layer.floorId}, {"level", item.layer.level},
                {"completed", false}, {"screenX", cursor.x}, {"screenY", cursor.y}});
        }
        candidatesProfile = observation.profileId; candidatesScene = observation.sceneName; ++candidatesRevision;
        nearbySelection = NearbySelection::Session{std::move(observation), intent, candidatesRevision};
        nearbySelectionResults.clear();
        json page = json::array();
        for (std::size_t i = 0; i < std::min<std::size_t>(100, markerCandidates.size()); ++i) page.push_back(markerCandidates[i]);
        event = {{"type", "markerCandidates"}, {"intent", intent == NearbySelection::Intent::Complete ? "complete" : "guide"},
            {"nearbySession", nearbySelection->source.session}, {"profileId", candidatesProfile}, {"sceneName", candidatesScene},
            {"selectionRevision", candidatesRevision}, {"candidates", page}, {"total", markerCandidates.size()},
            {"hasMore", page.size() < markerCandidates.size()}, {"gamepad", gamepad}, {"gameHwnd", nearbySelection->source.gameHwnd}};
    }
    StructuredLogger::Record("info", "gamepad", "nearby-candidates", "intent=" + event.at("intent").get<std::string>() +
        " revision=" + std::to_string(event.at("selectionRevision").get<std::uint64_t>()) +
        " count=" + std::to_string(event.at("total").get<std::size_t>()));
    if (publish) PublishMarkerEvent(event);
    return event;
}
void DrawItemBase::NotifyNearby(const std::string& message, const std::string& outcome) {
    StructuredLogger::Record("info", "gamepad", "nearby-result", outcome);
    Notification::AddInfo(NotificationDatas(message, 4));
    PublishMarkerEvent({{"type", "markerNearbyNotice"}, {"message", message}, {"outcome", outcome}});
}
void DrawItemBase::ClearMarkerCandidates(bool force) {
    bool changed;
    {
        std::scoped_lock lock(markerCandidatesMutex);
        // Overlay visibility cleanup must not expire a user's reading time.
        // Submit independently requires the registered window and a fresh fix.
        if (nearbySelection && !force) return;
        nearbySelection.reset(); nearbySelectionResults.clear();
        changed = !markerCandidates.empty(); markerCandidates = json::array();
        candidatesProfile.clear(); candidatesScene.clear(); ++candidatesRevision;
    }
    if (changed) PublishMarkerEvent({{"type", "markerSelectionCleared"}});
}
void DrawItemBase::UpdateMarkerContext(const std::string& sceneName) {
    bool changed;
    { std::scoped_lock lock(markerCandidatesMutex); changed = !candidatesScene.empty() && candidatesScene != sceneName; }
    if (changed) ClearMarkerCandidates(true);
}

void DrawItemBase::SelectMarker(const std::string& scene, const ItemDatas& item, POINT desktopPosition, const std::string& profileId) {
    const auto profile = profileId.empty() ? MarkerProfile() : profileId;
    if (profile != MarkerProfile()) return;
    PublishMarkerEvent({{"type", "markerSelected"}, {"profileId", profile}, {"sceneName", scene},
        {"pointId", item.itemId}, {"nameId", item.nameId}, {"stateId", item.layer.stateId},
        {"countryId", item.layer.countryId}, {"floorId", item.layer.floorId}, {"level", item.layer.level},
        {"completed", IsPointCompleted(scene, item)}, {"screenX", desktopPosition.x}, {"screenY", desktopPosition.y}});
}

void DrawItemBase::SetGuideWindow(HWND window, const json& registration) {
    std::scoped_lock lock(markerGuideMutex);
    markerGuideWindow = window;
    ++markerGuideRegistrationRevision;
    markerGuideRegistration = window ? registration : json::object();
    if (window) markerGuideRegistration["hwnd"] = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(window));
}
json DrawItemBase::FocusedGuideWindow() {
    const auto registration = VisibleGuideWindow();
    if (registration.empty()) return registration;
    const auto window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(registration.at("hwnd").get<std::uint64_t>()));
    return GetForegroundWindow() == window ? registration : json::object();
}
json DrawItemBase::VisibleGuideWindow() {
    std::scoped_lock lock(markerGuideMutex);
    return markerGuideWindow && IsWindow(markerGuideWindow) && IsWindowVisible(markerGuideWindow) && !IsIconic(markerGuideWindow)
        ? markerGuideRegistration : json::object();
}
bool DrawItemBase::IsMarkerGameFocused(HWND game) { return game && GetForegroundWindow() == game; }
bool DrawItemBase::IsMarkerDisplayContext(HWND game) {
    const auto foreground = GetForegroundWindow();
    if (game && foreground == game) return true;
    if (MapToolsBridge::Shared().FocusedHost(game, MarkerProfile())) return true;
    if (RouteGamepadBridge::Shared().FocusedHost(game)) return true;
    if (RouteGamepadBridge::Shared().ReturnDisplay(game, MarkerProfile()).visible) return true;
    return !FocusedGuideWindow().empty();
}
