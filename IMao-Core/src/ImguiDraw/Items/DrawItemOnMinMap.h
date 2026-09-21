#pragma once
#include "../../Runtime/OverlayMotion.h"
#include "vector"
#include "../../Coordinate/locationCalculator/ScreenCoordinate.h"
#include "DrawItemBase.h"


class DrawItemOnMinMap
{
public:
	static void DrawItemsOnMinMap(const RECT& rect, const ItemMarkerFrame& frame, const OverlayScreenTransform& motion = {});
	// Marker drawing cost since the last take, for the render loop's own two-second report.
	// candidates counts every marker the frame carried (completed ones included, because the
	// nearby update keeps them and the draw is what drops them); drawnIcons counts the icons
	// BuildMarkerLayout actually produced, so the pair is what shows a completion disappearing.
	// The millisecond fields are summed durations; divide by draws. Render thread only.
	struct MarkerRenderStats {
		std::uint64_t draws = 0;
		std::uint64_t candidates = 0;
		std::uint64_t drawnIcons = 0;
		double drawMs = 0.0;
		double layoutMs = 0.0;
		double textureLookupMs = 0.0;
	};
	static MarkerRenderStats TakeMarkerRenderStats();
	static void UpdatePlayerNearItemsData(HWND& hwnd, Coordinate& playerROC, float minMapRadius, int SceneId, double terrainScale = kNominalMinimapTerrainScale);
	static void UpdatePlayerNearItemsData(RECT &w_Rect, Coordinate &playerROC, float minMapRadius, int SceneId, double terrainScale = kNominalMinimapTerrainScale);
	static void SavePlayerNearItemPoint(const ItemMarkerFrame& frame, const OverlayScreenTransform& motion = {},
        bool gamepad = false, std::uint64_t gameHwnd = 0);
    static nlohmann::json HandlePlayerNearbyAction(bool guide, bool gamepad, std::uint64_t gameHwnd, bool publish = true);
    static ItemMarkerFrame Snapshot();
	static void ClearNearItemsData();

private:
	static std::mutex markerMutex;
	static std::vector<ItemDatas> nearItemsDatas;
	static std::string senceName;
	static std::shared_ptr<const std::vector<ItemsDatas>> itemsDatas_StoragePtr;

	static std::vector<ItemDatas> GetAndFilterItemsData(const RECT& rect, const Coordinate& playerROC, float minMapRadius, double terrainScale);
	static bool GetBasicDataBySenceId(int senceId);
};
