#pragma once
#include <string>
#include <nlohmann/json.hpp>
#include "Windows.h"
#include "../ImGuiOverWindows.h"
#include "../../Coordinate/CoordinateStruct.h"
#include <thread>
#include <atomic>
#include <functional>
#include <wrl/client.h>
using json = nlohmann::json;

#include "../../Domain/MapData.h"
#include "../../Runtime/NearbySelection.h"

struct ItemTextureData {
	std::string nameId;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> texture;

	ItemTextureData(std::string nameId = "", const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& texture = nullptr)
		: nameId(nameId), texture(texture) {
	}
};


class DrawItemBase {
public:
	static void Initi();
	static void Shutdown();
	static void RenderPointCircle(ImTextureID texture, ImVec2 position, float radius, float transparency, ImColor circleColor);
	static void AddItemDataFromJson(std::string itemId);
	static void ClearItemData(std::string itemId);
	static void SaveItemPoint(std::string scene, ItemDatas itemDatas);
	static void RemoveSavedItemPoint(std::string scene, ItemDatas itemDatas);
	static bool IsValidItemNameId(std::string itemNameId);
	// Kuro map icons are synchronized outside the DLL so new item types do not
	// require adding a new Windows resource for every upstream update.
	static std::string GetExternalIconPath(const std::string& itemNameId);
	static std::vector<std::string> GetFilteredPoints(std::string scene, std::string nameId);
    static bool IsPointCompleted(const std::string& scene, const ItemDatas& item);
    static json HandleMarkerCommand(const json& command);
    static void SetMarkerEventCallback(std::function<void(const json&)> callback);
    static void PublishMarkerEvent(json event);
    static void PublishMarkerCandidates(const std::string& profileId, const std::string& sceneName, json candidates,
        bool gamepad = false, std::uint64_t gameHwnd = 0);
    static void PublishNearbyCandidates(NearbySelection::Observation observation, NearbySelection::Intent intent, bool gamepad);
    static json CompleteNearbySingle(const NearbySelection::Observation& initial);
    static std::uint64_t MarkerFilterRevision();
    static void NotifyNearby(const std::string& message, const std::string& outcome);
    static void UpdateMarkerContext(const std::string& sceneName);
    static void ClearMarkerCandidates(bool force = false);
    static void SelectMarker(const std::string& scene, const ItemDatas& item, POINT desktopPosition, const std::string& profileId = "");
    static void SetGuideWindow(HWND window, const json& registration = json::object());
    static json FocusedGuideWindow();
    static json VisibleGuideWindow();
    static bool IsMarkerDisplayContext(HWND game);
    static bool IsMarkerGameFocused(HWND game);
    static std::string MarkerProfile();

	static std::vector<ItemTextureData> itemsTextureData;
	static json itemsJsonData_World;
	static json itemsJsonData_Tethys;
	static json itemsJsonData_Fabricatorium;
	static json itemsJsonData_Avinoleum;
	static json itemsJsonData_Lahai;
	static json itemsJsonData_LowerVault;
	static json itemsJsonData_Darkplain;
	static json itemsJsonData_TimeRiftRuins;
	static std::shared_ptr<const std::vector<ItemsDatas>> GetSceneItemsSnapshot(int sceneId);

private:
	static json& GetSavedItemPoints();
	static void LoadItemsjson();
	static void Thread_ReadSavedPointsJson();
	static bool FindItemJsonData(int sceneId, json*& itemJsonData);
	//static json savedItemPoints;
	static std::thread thread_ReadSavedPointsJson;
	static std::atomic_bool savedPointsThreadStop;
    static std::filesystem::path savedJsonPath;
};

