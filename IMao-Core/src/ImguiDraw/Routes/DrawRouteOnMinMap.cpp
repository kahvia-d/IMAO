#include "DrawRouteOnMinMap.h"
#include "LoadEditRouteData.h"
#include "../../Coordinate/locationCalculator/ScreenCoordinate.h"
#include "../../Runtime/RouteGeometry.h"
#include "../../Runtime/RoutePlanningService.h"
#include "../Items/DrawItemBase.h"
using namespace std;
using namespace cv;

vector<RouteDatas> DrawRouteOnMinMap::routesDatas;
mutex DrawRouteOnMinMap::routeMutex;

void DrawRouteOnMinMap::GetRoutePointsScreen(const RECT& rect ,const Coordinate& playerROC, float minMapRadius, int senceId, double terrainScale) {
	DrawRouteOnMinMap::ClearRountsData();
	lock_guard<mutex> lock(routeMutex);

	for (const auto& routeDatas : LoadEditRouteData::GetRoutesSnapshot()) {
		if (routeDatas.senceId != senceId) continue;
		vector<Coordinate> routePointsScreen;

		for (const auto routePointROC : routeDatas.routePointsROC) {
			// Offscreen endpoints may still form a segment crossing the minimap.
			// Retain every endpoint, then clip original segments at draw time.
			routePointsScreen.push_back(ScreenCoordinate::ItemScreenCoordinateOnMinMap(rect, routePointROC, playerROC, terrainScale));
		}

		if (routePointsScreen.size() >= 2)
			DrawRouteOnMinMap::routesDatas.push_back(RouteDatas("name", routeDatas.senceId, vector<Coordinate>(), routePointsScreen));
	}
}



void DrawRouteOnMinMap::DrawRoute(const std::vector<RouteDatas>& frame, int sceneId, const OverlayScreenTransform& motion, Coordinate clipCenter, double clipRadius) {

	if (frame.empty() || !(clipRadius > 0.0))
		return;

	const auto visibility = RoutePlanningService::DrawingVisibility();
	for (const auto& routeDatas : frame) {
		if (!visibility.Allows(routeDatas, true)) continue;
		if (routeDatas.automatic && routeDatas.profileId != DrawItemBase::MarkerProfile()) continue;

		if (routeDatas.senceId != sceneId)
			continue;

		const auto& screenPoints = routeDatas.routePointsScreenCoord;

		auto draw = ImGui::GetBackgroundDrawList();
		for (std::size_t i = 0; i + 1 < screenPoints.size(); ++i) {
			const auto first = motion.Apply(screenPoints[i]);
			const auto second = motion.Apply(screenPoints[i + 1]);
			const auto segment = AutoRoute::ClipCircle(first, second, clipCenter, clipRadius);
			if (!segment) continue;
			ImVec2 p1(static_cast<float>(segment->first.x), static_cast<float>(segment->first.y));
			ImVec2 p2(static_cast<float>(segment->second.x), static_cast<float>(segment->second.y));
			const ImU32 color = !routeDatas.automatic ? IM_COL32(255, 0, 0, 255) : routeDatas.preview ?
				IM_COL32(102, 201, 222, 190) : routeDatas.emphasized ? IM_COL32(255, 193, 73, 255) : IM_COL32(81, 168, 209, 210);
			const float thickness = routeDatas.emphasized ? 3.0f : routeDatas.automatic ? 2.0f : 1.5f;
			if (routeDatas.automatic && routeDatas.preview) {
				const float length = std::hypot(p2.x - p1.x, p2.y - p1.y);
				for (float d = 0; d < length; d += 12.0f) {
					const float end = std::min(d + 7.0f, length);
					draw->AddLine(ImVec2(p1.x + (p2.x - p1.x) * d / length, p1.y + (p2.y - p1.y) * d / length),
						ImVec2(p1.x + (p2.x - p1.x) * end / length, p1.y + (p2.y - p1.y) * end / length), color, thickness);
				}
			} else draw->AddLine(p1, p2, color, thickness);
		}
	}
}


std::vector<RouteDatas> DrawRouteOnMinMap::Snapshot() { std::scoped_lock lock(routeMutex); return routesDatas; }
