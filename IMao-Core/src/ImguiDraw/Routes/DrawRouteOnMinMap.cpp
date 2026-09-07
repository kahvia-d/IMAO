#include "DrawRouteOnMinMap.h"
#include "LoadEditRouteData.h"
#include "../../Coordinate/locationCalculator/ScreenCoordinate.h"
using namespace std;
using namespace cv;

vector<RouteDatas> DrawRouteOnMinMap::routesDatas;
mutex DrawRouteOnMinMap::routeMutex;

Coordinate minMapCenterScreenCoordinate;

void DrawRouteOnMinMap::GetRoutePointsScreen(const RECT& rect ,const Coordinate& playerROC, float minMapRadius, int senceId, double terrainScale) {
	DrawRouteOnMinMap::ClearRountsData();
	lock_guard<mutex> lock(routeMutex);

	for (const auto& routeDatas : LoadEditRouteData::GetRoutesSnapshot()) {
		if (routeDatas.senceId != senceId) continue;
		vector<Coordinate> routePointsScreen;

		for (const auto routePointROC : routeDatas.routePointsROC) {
			if (abs(routePointROC.x - playerROC.x) < 120 and abs(routePointROC.y - playerROC.y) < 120) {
				Coordinate routePointScreen = ScreenCoordinate::ItemScreenCoordinateOnMinMap(rect, routePointROC, playerROC, terrainScale);
				minMapCenterScreenCoordinate = ScreenCoordinate::MinMapCircleCenterScreenCoordinate(rect);

				float twoPointDistance = CalculatePointDistance(routePointScreen, minMapCenterScreenCoordinate);
				if (twoPointDistance <= minMapRadius) {
					routePointsScreen.push_back(routePointScreen);
				}
			}
		}

		if (routePointsScreen.size() >= 2)
			DrawRouteOnMinMap::routesDatas.push_back(RouteDatas("name", routeDatas.senceId, vector<Coordinate>(), routePointsScreen));
	}
}



void DrawRouteOnMinMap::DrawRoute(const std::vector<RouteDatas>& frame, int sceneId, const OverlayScreenTransform& motion) {

	if (frame.empty())
		return;

	for (const auto& routeDatas : frame) {

		if (routeDatas.senceId != sceneId)
			continue;

		const auto& screenPoints = routeDatas.routePointsScreenCoord;

		auto draw = ImGui::GetBackgroundDrawList();
		for (int i = 0; i < screenPoints.size() - 1; i++) {
			const auto first = motion.Apply(screenPoints[i]);
			const auto second = motion.Apply(screenPoints[i + 1]);
			ImVec2 p1 = ImVec2(first.x, first.y);
			ImVec2 p2 = ImVec2(second.x, second.y);
			ImU32 color = IM_COL32(255, 0, 0, 255);

			draw->AddLine(p1, p2, color, 1.5);
		}
	}
}


std::vector<RouteDatas> DrawRouteOnMinMap::Snapshot() { std::scoped_lock lock(routeMutex); return routesDatas; }
