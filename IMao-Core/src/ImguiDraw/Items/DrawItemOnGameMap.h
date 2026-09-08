#pragma once
#include "../../Runtime/OverlayMotion.h"
#include "../../Runtime/FrameState.h"
#include<vector>
#include "../../Coordinate/locationCalculator/ScreenCoordinate.h"
#include "DrawItemBase.h"

class DrawItemOnGameMap
{
public:
	static bool HasVisibleItems();
	static void UpdateCenterPointNearItemsData(const Coordinate& validGameMapcenterPointROC,const std::vector<cv::Point2f>& captureCorners, const RECT& rect, int senceId);
	static void DrawItemsOnGameMap(const RECT& rect, const HWND& hwnd, const ItemMarkerFrame& frame, const OverlayScreenTransform& motion = {}, const PresentedOverlayFrame* presented = nullptr);
    static ItemMarkerFrame Snapshot();
	static void ClearNearItemsData();
	static void SetVisibleSavedPoints(bool value) {
		visibleSavedPoints = value;
	}
private:
	static std::vector<ItemDatas> centerPointNearItemsData;
	static std::string senceName;
	static std::shared_ptr<const std::vector<ItemsDatas>> itemsDatas_StoragePtr;

	static std::vector<ItemDatas> GetAndFilterItemsData(const Coordinate& gameMapcenterPointRC, const std::vector<cv::Point2f>& captureCorners, const RECT& rect);
	static bool GetBasicDataBySenceId(int senceId);
	static std::atomic_bool visibleSavedPoints;
	static std::mutex PointNearItemsDataMutex;
};

