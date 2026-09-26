#include "ScreenCoordinate.h"
#include "../../util.h"

using namespace cv;
using namespace std;
Coordinate ScreenCoordinate::MinMapCircleCenterScreenCoordinate(const RECT &w_Rect) {
    return GetMinimapProjectionGeometry(w_Rect).center;
}

// GetClientRect leaves left/top at zero, so right/bottom are the client size. The anchors inside the
// box decide where it lands; HudLayout.h records why each widget hugs the edge it does. ScreenRect
// itself is inline in the header so the geometry tests can pin the boxes without this translation unit.

RectangularAreaScreenLocation ScreenCoordinate::SpecifyScreenCoordinate(const RECT& w_Rect, const hud::Box& box) {
    const Rect area = ScreenRect(w_Rect, box);

    Coordinate topPoint(area.x + area.width / 2.0, area.y);
    Coordinate bottomPoint(area.x + area.width / 2.0, area.y + area.height);
    Coordinate leftPoint(area.x, area.y + area.height / 2.0);
    Coordinate rightPoint(area.x + area.width, area.y + area.height / 2.0);

    RectangularAreaScreenLocation areaLocation(leftPoint, rightPoint, topPoint, bottomPoint);

    return areaLocation;
}

RectangularAreaScreenLocation ScreenCoordinate::SpecifyScreenCoordinate(const HWND& hwnd, const hud::Box& box) {
    RECT rect;
    GetClientRect(hwnd, &rect);

    return ScreenCoordinate::SpecifyScreenCoordinate(rect, box);
}

//已知item相对世界原点的坐标  玩家相对世界原点的坐标 小地图中心点在屏幕的坐标，那么item的屏幕坐标为
//item相对玩家的坐标.x * 小地图相对地图的缩放比例 + 小地图中心点在屏幕的坐标.x
//小地图中心点在屏幕的坐标.y - item相对玩家的坐标.y * 小地图相对地图的缩放比例
//缩放比例与实际整数裁剪和本次识别校准的地形比例保持一致。

Coordinate ScreenCoordinate::ItemScreenCoordinateOnMinMap(const HWND& hwnd, const Coordinate& itemROC,
    const Coordinate& playerROC, double terrainScale) {
    RECT rect{};
    if (!GetClientRect(hwnd, &rect)) return {};
    return ItemScreenCoordinateOnMinMap(rect, itemROC, playerROC, terrainScale);
}

Coordinate ScreenCoordinate::ItemScreenCoordinateOnMinMap(const RECT& rect, const Coordinate& itemROC,
    const Coordinate& playerROC, double terrainScale) {
    const auto geometry = GetMinimapProjectionGeometry(rect, terrainScale);
    return {geometry.center.x + (itemROC.x - playerROC.x) * geometry.pixelsPerMapUnit,
        geometry.center.y - (itemROC.y - playerROC.y) * geometry.pixelsPerMapUnit};
}


Coordinate ScreenCoordinate::ItemScreenCoordinateOnMap(const Coordinate &gameMapCenterPointImgMapROC,const Coordinate &itemROC,const vector<Point2f> &captureCorners,const RECT &w_rect) {
    if (captureCorners.empty()) {
        return Coordinate(-1, -1);
    }

    const Rect mapCenterArea = ScreenRect(w_rect, hud::kMapCenterArea);

    float scalingFactor =  static_cast<float>(mapCenterArea.width / (captureCorners[2].x - captureCorners[0].x));

   // Coordinate gameMapCenterPointRWOC = RelativeCoordinates::ImgMapCoordToRWOC(gameMapCenter);
   Coordinate ScreenCenter(w_rect.right / 2, w_rect.bottom / 2);

   float itemScreenCoordinateOnMap_x = ScreenCenter.x + (itemROC.x - gameMapCenterPointImgMapROC.x) * scalingFactor;
   float itemScreenCoordinateOnMap_y = ScreenCenter.y - (itemROC.y - gameMapCenterPointImgMapROC.y) * scalingFactor;

   return Coordinate(itemScreenCoordinateOnMap_x, itemScreenCoordinateOnMap_y);
}
