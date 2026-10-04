#include "DrawRouteOnMinMap.h"
#include "../../Coordinate/locationCalculator/ScreenCoordinate.h"
#include "../../Runtime/RouteGeometry.h"
#include "../../Runtime/RoutePlanningService.h"
#include "../Items/DrawItemBase.h"
#include "DrawFreePointBadge.h"
using namespace std;
using namespace cv;
void DrawRouteOnMinMap::DrawFreeMarkers(const std::vector<FreeRouteMarker>& markers,int sceneId,
    const OverlayScreenTransform& motion,Coordinate center,double clipRadius) {
    const auto* scene=Scene::Find(sceneId);if(!scene||clipRadius<=0)return;
    const auto visibility=RoutePlanningService::DrawingVisibility();
    for(const auto& marker:markers) {
        if(marker.profileId!=DrawItemBase::MarkerProfile()||marker.point.layer.stateId!=scene->kuroStateId)continue;
        RouteDatas identity("",sceneId);identity.routePlanId=marker.routeId;identity.profileId=marker.profileId;
        identity.orderRevision=marker.orderRevision;if(!visibility.Allows(identity,true))continue;
        if(LayeredMap::RoleFor(marker.point)==LayeredMap::MarkerRole::Hidden)continue;
        const auto position=motion.Apply(marker.point.screenCoordiante);
        const auto label=AutoRoute::FreePointLabel(marker.point.freeIcon,marker.order);
        DrawFreePointBadgeClipped(ImGui::GetBackgroundDrawList(),{static_cast<float>(position.x),static_cast<float>(position.y)},label,marker.current,center,clipRadius);
    }
}

void DrawRouteOnMinMap::DrawRoute(const std::vector<RouteDatas>& frame, int sceneId, const OverlayScreenTransform& motion, Coordinate clipCenter, double clipRadius) {

	if (frame.empty() || !(clipRadius > 0.0))
		return;

	const auto visibility = RoutePlanningService::DrawingVisibility();
	for (const auto& routeDatas : frame) {
		if (!visibility.Allows(routeDatas, true)) continue;
		if (routeDatas.profileId != DrawItemBase::MarkerProfile()) continue;

		if (routeDatas.senceId != sceneId)
			continue;

		const auto& screenPoints = routeDatas.routePointsScreenCoord;

		auto draw = ImGui::GetBackgroundDrawList();
		for (std::size_t i = 0; i + 1 < screenPoints.size(); ++i) {
			const auto first = i == 0 && (routeDatas.emphasized || routeDatas.previousTarget)
                ? clipCenter : motion.Apply(screenPoints[i]);
			const auto second = motion.Apply(screenPoints[i + 1]);
			const auto segment = AutoRoute::ClipCircle(first, second, clipCenter, clipRadius);
			if (!segment) continue;
			ImVec2 p1(static_cast<float>(segment->first.x), static_cast<float>(segment->first.y));
			ImVec2 p2(static_cast<float>(segment->second.x), static_cast<float>(segment->second.y));
            const ImU32 color = routeDatas.handDrawn ? IM_COL32(102, 201, 222, 235) :
                routeDatas.previousTarget ? IM_COL32(172, 180, 190, 195) : routeDatas.preview ?
				IM_COL32(102, 201, 222, 190) : routeDatas.emphasized ? IM_COL32(255, 193, 73, 255) : IM_COL32(81, 168, 209, 210);
			const float thickness = routeDatas.handDrawn ? 3.5f : routeDatas.emphasized ? 3.0f : 2.0f;
            if (routeDatas.handDrawn) draw->AddLine(p1, p2, color, thickness);
            else if (routeDatas.preview || routeDatas.previousTarget) {
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
