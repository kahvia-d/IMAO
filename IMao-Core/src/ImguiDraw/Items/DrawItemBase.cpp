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
#include "../../Runtime/RoutePlanningService.h"
#include "../../Runtime/MarkerGuideProtocol.h"
#include <functional>
#include "../../Runtime/StructuredLogger.h"
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
static SceneItemStore<ItemsDatas> selectedItems;
thread DrawItemBase::thread_ReadSavedPointsJson;
std::atomic_bool DrawItemBase::savedPointsThreadStop = false;
std::filesystem::path DrawItemBase::savedJsonPath;

static std::unique_ptr<MarkerCompletionStore> markerStore;
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
static bool LoadExternalKuroRuntimeJson(json& jsonData, const char* sceneName);

Coordinate KuroLocationToIdentifyCoordinate(const json& location) {
    return KuroPositionToGameCoordinates(location.at("x").get<double>(), location.at("y").get<double>());
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
    markerStore = std::make_unique<MarkerCompletionStore>(directory);
    markerIdentities.clear();
    for (const auto sceneId : Scene::sceneIds) {
        json* source = nullptr;
        if (!FindItemJsonData(sceneId, source) || !source || !source->is_array()) continue;
        const auto scene = Scene::SceneIdToName(sceneId);
        for (const auto& category : *source) {
            for (const auto& location : category.value("location", json::array())) {
                if (!location.contains("id") || !location.at("id").is_string()) continue;
                const auto id = location.at("id").get<std::string>();
                const int state = location.value("stateId", MarkerCompletionStore::SceneState(scene));
                markerIdentities[std::to_string(state) + ":" + id] = {{"sceneName", scene},
                    {"nameId", category.value("id", "")}, {"stateId", state}, {"pointId", id}};
            }
        }
    }
    RoutePlanningService::Initialize();
}

void DrawItemBase::Shutdown() {
    RoutePlanningService::Shutdown();
    SetMarkerEventCallback({});
    SetGuideWindow(nullptr);
    markerStore.reset();
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
    if (!LoadJson(itemsJsonData_Tethys, L"ITEMSJSON_Tethys") ||
        !LoadJson(itemsJsonData_World, L"ITEMSJSON_World") ||
        !LoadJson(itemsJsonData_Fabricatorium, L"ITEMSJSON_Fabricatorium") ||
        !LoadJson(itemsJsonData_Avinoleum, L"ITEMSJSON_Avinoleum") ||
        !LoadJson(itemsJsonData_Lahai, L"ITEMSJSON_Lahai"))
        throw std::runtime_error("Required map item resources are missing or invalid");
	LoadExternalKuroRuntimeJson(itemsJsonData_LowerVault, "LowerVault");
	LoadExternalKuroRuntimeJson(itemsJsonData_Darkplain, "Darkplain");
	LoadExternalKuroRuntimeJson(itemsJsonData_TimeRiftRuins, "TimeRiftRuins");
}

static bool LoadExternalKuroRuntimeJson(json& jsonData, const char* sceneName) {
    try {
        const int sceneId = Scene::SceneNameToId(sceneName);
        if (!Scene::IsRuntimeApproved(sceneId)) {
            cerr << "Kuro map scene is not release-approved: " << sceneName << endl;
            return false;
        }
        const fs::path path = fs::path(GetCurrentPath()) / "Assets" / "KuroMap" / "runtime" /
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
            const fs::path manifestPath = fs::path(GetCurrentPath()) / "Assets" / "KuroMap" / "icon-manifest.json";
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
                if (candidate.is_absolute() || candidate.empty()) {
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
                   selectedItems.Add(sceneId, ItemsDatas(nameId, std::move(itemsDatas)));
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
    selectedItems.Remove(itemId);
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
    return markerStore ? markerStore->CompletedIds(scene, nameId) : vector<string>{};
}

bool DrawItemBase::IsPointCompleted(const string& scene, const ItemDatas& item) {
    return markerStore && markerStore->Completed(scene, item.nameId, item.itemId);
}

std::string DrawItemBase::MarkerProfile() { return markerStore ? markerStore->Profile() : "local"; }

json DrawItemBase::HandleMarkerCommand(const json& command) {
    if (!markerStore) return {{"accepted", false}, {"message", "marker-store-unavailable"}, {"data", json::object()}};
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
            {"selectionRevision", candidatesRevision}, {"total", markerCandidates.size()}, {"hasMore", offset + page.size() < markerCandidates.size()}}}};
    }
    if (type == "markerSetCompletion" && command.contains("stateId") && command.contains("pointId")) {
        const auto key = std::to_string(command.at("stateId").get<int>()) + ":" + command.at("pointId").get<std::string>();
        const auto identity = markerIdentities.find(key);
        if (identity == markerIdentities.end()) return {{"accepted", false}, {"message", "unknown-public-point"}, {"data", json::object()}};
        for (const auto& [name, value] : identity->second.items()) normalized[name] = value;
    }
    if ((type == "markerApplyRemote" || type == "markerInitializeSync") && !normalized.contains("points")) {
        const int state = command.at("stateId").get<int>();
        normalized["points"] = json::array();
        for (const auto& [key, identity] : markerIdentities)
            if (identity.at("stateId") == state) normalized["points"].push_back(identity);
    }
    auto result = markerStore->Execute(normalized);
    if (result.value("accepted", false)) {
        if (type == "markerSetCompletion" || type == "markerApplyRemote" || type == "markerInitializeSync" ||
            type == "markerResolveConflict" || type == "markerCopyLocalProgress") {
            json event = {{"type", "markerCompletionChanged"}, {"profileId", markerStore->Profile()},
                {"source", type == "markerSetCompletion" || type == "markerCopyLocalProgress" ? "local" : "cloud"},
                {"revision", result.at("data").value("revision", std::uint64_t{})}};
            if (result.at("data").contains("point")) event["point"] = result.at("data").at("point");
            for (const auto& [name, value] : guideContext.items()) event[name] = value;
            PublishMarkerEvent(std::move(event));
        } else if (type == "markerSelectProfile") {
            ClearMarkerCandidates();
            PublishMarkerEvent({{"type", "markerSelectionCleared"}});
            PublishMarkerEvent({{"type", "markerProfileChanged"}, {"profileId", markerStore->Profile()}});
        }
    }
    return result;
}

void DrawItemBase::SetMarkerEventCallback(std::function<void(const json&)> callback) {
    std::scoped_lock lock(markerEventMutex);
    markerEventCallback = std::move(callback);
}

void DrawItemBase::PublishMarkerEvent(json event) {
    const auto type = event.value("type", "");
    if (type == "markerCompletionChanged" || type == "markerProfileChanged") RoutePlanningService::OnMarkerChanged();
    std::function<void(const json&)> callback;
    { std::scoped_lock lock(markerEventMutex); callback = markerEventCallback; }
    if (callback) callback(event);
}

void DrawItemBase::PublishMarkerCandidates(const std::string& profileId, const std::string& sceneName, json candidates) {
    json event;
    {
        std::scoped_lock lock(markerCandidatesMutex);
        markerCandidates = std::move(candidates);
        candidatesProfile = profileId; candidatesScene = sceneName; ++candidatesRevision;
        json page = json::array();
        for (std::size_t index = 0; index < std::min<std::size_t>(100, markerCandidates.size()); ++index) page.push_back(markerCandidates[index]);
        event = {{"type", "markerCandidates"}, {"intent", "complete"}, {"profileId", profileId},
            {"candidates", page}, {"selectionRevision", candidatesRevision}, {"total", markerCandidates.size()}, {"hasMore", page.size() < markerCandidates.size()}};
    }
    PublishMarkerEvent(std::move(event));
}
void DrawItemBase::ClearMarkerCandidates() {
    bool changed;
    {
        std::scoped_lock lock(markerCandidatesMutex);
        changed = !markerCandidates.empty(); markerCandidates = json::array();
        candidatesProfile.clear(); candidatesScene.clear(); ++candidatesRevision;
    }
    if (changed) PublishMarkerEvent({{"type", "markerSelectionCleared"}});
}
void DrawItemBase::UpdateMarkerContext(const std::string& sceneName) {
    bool changed;
    { std::scoped_lock lock(markerCandidatesMutex); changed = !candidatesScene.empty() && candidatesScene != sceneName; }
    if (changed) ClearMarkerCandidates();
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
    return !FocusedGuideWindow().empty();
}
