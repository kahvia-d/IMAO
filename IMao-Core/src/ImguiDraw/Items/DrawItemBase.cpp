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

static std::shared_mutex g_jsonMutex;          // 读写锁 
static std::filesystem::file_time_type g_lastTime; // 上一次修改时间 
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
    savedJsonPath = StructuredLogger::ApplicationDataDirectory() / "SavedPoints" / "account_1.json";
    fs::create_directories(savedJsonPath.parent_path());
    const auto legacy = fs::path(GetCurrentPath()) / "SavedPoints" / "account_1.json";
    if (!fs::exists(savedJsonPath) && fs::exists(legacy)) fs::copy_file(legacy, savedJsonPath);
    // Load before accepting any write; the watcher only handles subsequent edits.
    if (fs::exists(savedJsonPath)) {
        try {
            ifstream input(savedJsonPath);
            json next = json::parse(input);
            if (!next.is_object()) throw std::runtime_error("Saved points must be an object");
            GetSavedItemPoints() = std::move(next);
        }
        catch (const std::exception& exception) {
            auto backup = savedJsonPath;
            backup += ".corrupt-" + std::to_string(GetTickCount64());
            fs::copy_file(savedJsonPath, backup);
            StructuredLogger::Record("error", "storage", "saved-points-corrupt", exception.what());
            GetSavedItemPoints() = json::object();
        }
        g_lastTime = fs::last_write_time(savedJsonPath);
    }

    savedPointsThreadStop = false;
    thread_ReadSavedPointsJson = std::thread(&DrawItemBase::Thread_ReadSavedPointsJson);
}

void DrawItemBase::Shutdown() {
    savedPointsThreadStop = true;
    if (thread_ReadSavedPointsJson.joinable()) {
        thread_ReadSavedPointsJson.join();
    }
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
                        tempItemDatas.layer.stateId = location.value("stateId", 0);
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
    try {
        unique_lock lock(g_jsonMutex);
        auto j = GetSavedItemPoints();

        auto& points = j[scene][itemDatas.nameId];
        if (points.is_array() && std::any_of(points.begin(), points.end(), [&](const json& point) {
            return point.value("id", "") == itemDatas.itemId;
        })) return;
        points.push_back({ {"id", itemDatas.itemId} });

        WriteTextAtomically(fs::path(savedJsonPath), j.dump(4));
        GetSavedItemPoints() = std::move(j);
        g_lastTime = filesystem::last_write_time(savedJsonPath);
    }
    catch (const exception& e) {
		Notification::AddError(NotificationDatas("DrawItemBase::SaveItemPoint: " + string(e.what()), 5));
    }
}

void DrawItemBase::RemoveSavedItemPoint(string scene, ItemDatas itemDatas) {
    try {
        unique_lock lock(g_jsonMutex);// 独占写 
        auto j = GetSavedItemPoints();

        auto& item_array = j[scene][itemDatas.nameId];
        for (auto it = item_array.begin(); it != item_array.end(); ++it) {
            if (it->at("id") == itemDatas.itemId) {
                item_array.erase(it);
                break;
            }
        }
        WriteTextAtomically(fs::path(savedJsonPath), j.dump(4));
        GetSavedItemPoints() = std::move(j);
        g_lastTime = filesystem::last_write_time(savedJsonPath);
    }catch (const exception& e) {
		Notification::AddError(NotificationDatas("DrawItemBase::RemoveSavedItemPoint: " + string(e.what()), 5));
    }
}

vector<string> DrawItemBase::GetFilteredPoints(string scene,string nameId) {
    vector<string> out;
    try {
        shared_lock lock(g_jsonMutex);
        const auto& j = GetSavedItemPoints();

        if (!j.contains(scene) ||
            !j[scene].is_object() ||
            !j[scene].contains(nameId) ||
            !j[scene][nameId].is_array()) {
            return out;
        }

        for (const auto& item : j[scene][nameId]) {
            if (item.contains("id") && item["id"].is_string()) {
                out.emplace_back(item["id"].get<string>());
            }
        }
    }
    catch (const exception& e) {
        cerr << "GetFilteredPoints exception: " << e.what() << '\n';
    }
    return out;
}

void DrawItemBase::Thread_ReadSavedPointsJson() {
    while (!savedPointsThreadStop.load()) {
        try {
            unique_lock lock(g_jsonMutex);
            if (fs::exists(savedJsonPath)) {
                auto t = fs::last_write_time(savedJsonPath);
                if (t != g_lastTime) {  // 只有变化才读 
                    ifstream file(savedJsonPath);
                    json next;
                    file >> next;
                    if (!next.is_object()) throw std::runtime_error("Saved points must be an object");
                    GetSavedItemPoints() = std::move(next);
                    g_lastTime = t;
                }
            }
        }
        catch (const exception& e) {
			Notification::AddError(NotificationDatas("JSON reload failed.", 3));
        }
        this_thread::sleep_for(chrono::milliseconds(50));
    }
}
