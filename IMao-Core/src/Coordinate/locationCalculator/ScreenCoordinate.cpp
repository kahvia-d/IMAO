#include "ScreenCoordinate.h"
#include "../../util.h"

using namespace cv;
using namespace std;
Coordinate ScreenCoordinate::MinMapCircleCenterScreenCoordinate(const RECT &w_Rect) {
    return GetMinimapProjectionGeometry(w_Rect).center;
}

//水平缩放因子*x，垂直缩放因子*y
RectangularAreaScreenLocation ScreenCoordinate::MinMapScreenCoordinate(HWND& w_hwnd) {
    double HorizontalFactor = 0;//水平缩放因子
    double VerticaFactor = 0;//垂直缩放因子

    CalculateWindowScalingFactors(w_hwnd, HorizontalFactor, VerticaFactor);

    Coordinate topPoint(GameWindowsScreenData::MinMapTop.x * HorizontalFactor,
        GameWindowsScreenData::MinMapTop.y * VerticaFactor);

    Coordinate bottomPoint(GameWindowsScreenData::MinMapBottom.x * HorizontalFactor,
        GameWindowsScreenData::MinMapBottom.y * VerticaFactor);

    Coordinate leftPoint(GameWindowsScreenData::MinMapLeft.x * HorizontalFactor,
        GameWindowsScreenData::MinMapLeft.y * VerticaFactor);

    Coordinate rightPoint(GameWindowsScreenData::MinMapRight.x * HorizontalFactor,
        GameWindowsScreenData::MinMapRight.y * VerticaFactor);

    RectangularAreaScreenLocation MPCSC(leftPoint, rightPoint, topPoint, bottomPoint);

    return MPCSC;
}


RectangularAreaScreenLocation ScreenCoordinate::SpecifyScreenCoordinate(const RECT& w_Rect, std::vector<Coordinate> specifyAreaScreenData) {
    double HorizontalFactor = 0;//水平缩放因子
    double VerticaFactor = 0;//垂直缩放因子

    CalculateWindowScalingFactors(w_Rect, HorizontalFactor, VerticaFactor);

    Coordinate topPoint(specifyAreaScreenData[0].x * HorizontalFactor,
        specifyAreaScreenData[0].y * VerticaFactor);

    Coordinate bottomPoint(specifyAreaScreenData[1].x * HorizontalFactor,
        specifyAreaScreenData[1].y * VerticaFactor);

    Coordinate leftPoint(specifyAreaScreenData[2].x * HorizontalFactor,
        specifyAreaScreenData[2].y * VerticaFactor);

    Coordinate rightPoint(specifyAreaScreenData[3].x * HorizontalFactor,
        specifyAreaScreenData[3].y * VerticaFactor);

    RectangularAreaScreenLocation SCSC(leftPoint, rightPoint, topPoint, bottomPoint);

    return SCSC;
}

RectangularAreaScreenLocation ScreenCoordinate::SpecifyScreenCoordinate(const HWND& hwnd, std::vector<Coordinate> specifyAreaScreenData) {
    RECT rect;
    GetClientRect(hwnd, &rect);

    return ScreenCoordinate::SpecifyScreenCoordinate(rect, specifyAreaScreenData);
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

    auto MapCenterAreaData = ScreenCoordinate::SpecifyScreenCoordinate(w_rect, GameWindowsScreenData::mapCenterAreaSrceenData);

    float scalingFactor =  (MapCenterAreaData.rightPoint.x - MapCenterAreaData.leftPoint.x)/ (captureCorners[2].x - captureCorners[0].x);

   // Coordinate gameMapCenterPointRWOC = RelativeCoordinates::ImgMapCoordToRWOC(gameMapCenter);
    Coordinate ScreenCenter(w_rect.right / 2, w_rect.bottom / 2);

   float itemScreenCoordinateOnMap_x = ScreenCenter.x + (itemROC.x - gameMapCenterPointImgMapROC.x) * scalingFactor;
   float itemScreenCoordinateOnMap_y = ScreenCenter.y - (itemROC.y - gameMapCenterPointImgMapROC.y) * scalingFactor;

   return Coordinate(itemScreenCoordinateOnMap_x, itemScreenCoordinateOnMap_y);
}
