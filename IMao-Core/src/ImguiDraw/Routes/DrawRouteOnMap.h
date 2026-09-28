#pragma once
#include "../../Runtime/OverlayMotion.h"
#include "vector"
#include "../ImGuiOverWindows.h"
#include "../../Coordinate/CoordinateStruct.h"
#include "../../util.h"
#include "../../Domain/MapData.h"

// Draws the route segments the frame carries. It owns no route state: the caller assembles the
// segment list from the route plans and hands it over, so there is nothing here to clear or to
// keep in sync with a store.
class DrawRouteOnMap{
public:
	static void DrawRoute(const std::vector<RouteDatas>& frame, int sceneId, const OverlayScreenTransform& motion = {}, const RECT& clipRect = {});
};
