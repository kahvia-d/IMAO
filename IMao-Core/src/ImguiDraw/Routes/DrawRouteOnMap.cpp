#include "DrawRouteOnMap.h"
#include "../../Coordinate/locationCalculator/ScreenCoordinate.h"
#include "../../Runtime/RouteGeometry.h"
#include "../../Runtime/RoutePlanningService.h"
#include "../Items/DrawItemBase.h"

using namespace std;
using namespace cv;

void DrawRouteOnMap::DrawRoute(const std::vector<RouteDatas>& frame, int sceneId, const OverlayScreenTransform& motion, const RECT& clipRect) {

	if (frame.empty())
		return;

	const auto size = ImGui::GetIO().DisplaySize;
	const double right = clipRect.right > clipRect.left ? clipRect.right : size.x;
	const double bottom = clipRect.bottom > clipRect.top ? clipRect.bottom : size.y;
	const auto visibility = RoutePlanningService::DrawingVisibility();
	for (const auto& routeDatas : frame) {
		if (!visibility.Allows(routeDatas)) continue;
		if (routeDatas.profileId != DrawItemBase::MarkerProfile()) continue;
		const auto& screenPoints = routeDatas.routePointsScreenCoord;

		if (routeDatas.senceId != sceneId) {
			continue;
		}
			
		auto draw = ImGui::GetBackgroundDrawList();
		for (std::size_t i = 0; i + 1 < screenPoints.size(); ++i) {
			const auto first = motion.Apply(screenPoints[i]);
			const auto second = motion.Apply(screenPoints[i + 1]);
			const auto segment = AutoRoute::ClipRectangle(first, second, 0.0, 0.0, right, bottom);
			if (!segment) continue;
			ImVec2 p1(static_cast<float>(segment->first.x), static_cast<float>(segment->first.y));
			ImVec2 p2(static_cast<float>(segment->second.x), static_cast<float>(segment->second.y));
            const ImU32 color = routeDatas.handDrawn ? IM_COL32(102, 201, 222, 235) :
                routeDatas.previousTarget ? IM_COL32(172, 180, 190, 195) : routeDatas.preview ?
				IM_COL32(102, 201, 222, 190) : routeDatas.emphasized ? IM_COL32(255, 193, 73, 255) : IM_COL32(81, 168, 209, 210);
			const float thickness = routeDatas.handDrawn ? 4.0f : routeDatas.emphasized ? 3.5f : 2.0f;
            if (routeDatas.handDrawn) {
                // Solid and heavy while drawing, with an arrow at each segment's midpoint: the
                // player is building an ordered path and needs to see which way it runs.
                draw->AddLine(p1, p2, color, thickness);
                const float length = std::hypot(p2.x - p1.x, p2.y - p1.y);
                if (length >= 24.0f) {
                    const float dx = (p2.x - p1.x) / length, dy = (p2.y - p1.y) / length;
                    const ImVec2 tip(p1.x + (p2.x - p1.x) * 0.5f, p1.y + (p2.y - p1.y) * 0.5f);
                    const float size = 7.0f, wing = 5.0f;
                    const ImVec2 back(tip.x - dx * size, tip.y - dy * size);
                    draw->AddLine(tip, ImVec2(back.x - dy * wing, back.y + dx * wing), color, thickness);
                    draw->AddLine(tip, ImVec2(back.x + dy * wing, back.y - dx * wing), color, thickness);
                }
            } else if (routeDatas.preview || routeDatas.previousTarget) {
				const float length = std::hypot(p2.x - p1.x, p2.y - p1.y);
				for (float d = 0; d < length; d += 14.0f) {
					const float end = std::min(d + 8.0f, length);
					draw->AddLine(ImVec2(p1.x + (p2.x - p1.x) * d / length, p1.y + (p2.y - p1.y) * d / length),
						ImVec2(p1.x + (p2.x - p1.x) * end / length, p1.y + (p2.y - p1.y) * end / length), color, thickness);
				}
			} else draw->AddLine(p1, p2, color, thickness);
		}
	}
}
