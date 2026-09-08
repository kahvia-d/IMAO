#pragma once
#include "../../Runtime/OverlayMotion.h"
#include "../../Coordinate/locationCalculator/MinimapProjectionGeometry.h"
#include "vector"
#include "../ImGuiOverWindows.h"
#include "../../Coordinate/CoordinateStruct.h"
#include "../../util.h"
#include "LoadEditRouteData.h"


class DrawRouteOnMinMap{
public:
	static void GetRoutePointsScreen(const RECT& rect,const Coordinate& playerROC, float minMapRadius, int senceId, double terrainScale = kNominalMinimapTerrainScale);
	static void ClearRountsData() {
		std::lock_guard<std::mutex> lock(routeMutex);
		routesDatas.clear();
	}

	static void DrawRoute(const std::vector<RouteDatas>& frame, int sceneId, const OverlayScreenTransform& motion = {}, Coordinate clipCenter = {}, double clipRadius = 0.0);
    static std::vector<RouteDatas> Snapshot();
private:
	static std::vector<RouteDatas> routesDatas;
	static std::mutex routeMutex;
};
