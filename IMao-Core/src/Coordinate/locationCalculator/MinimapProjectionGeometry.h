#pragma once
#include "../CoordinateStruct.h"

inline constexpr double kNominalMinimapTerrainScale = 194.0 / 184.0;

struct MinimapProjectionGeometry {
    Coordinate center;
    double pixelsPerMapUnit = 0.0;
    int left = 0, top = 0, width = 0, height = 0;
};

// Use exactly the integer crop supplied to the 184px minimap localizer.
// A screen-width affine formula disagrees with this geometry across resolutions.
inline MinimapProjectionGeometry GetMinimapProjectionGeometry(const RECT& rect,
    double terrainScale = kNominalMinimapTerrainScale) {
    const double sx = rect.right / GameWindowsScreenData::w_width;
    const double sy = rect.bottom / GameWindowsScreenData::w_height;
    MinimapProjectionGeometry result;
    result.left = static_cast<int>(GameWindowsScreenData::MinMapLeft.x * sx);
    result.top = static_cast<int>(GameWindowsScreenData::MinMapTop.y * sy);
    result.width = static_cast<int>((GameWindowsScreenData::MinMapRight.x - GameWindowsScreenData::MinMapLeft.x) * sx);
    result.height = static_cast<int>((GameWindowsScreenData::MinMapBottom.y - GameWindowsScreenData::MinMapTop.y) * sy);
    result.center = {result.left + result.width / 2.0, result.top + result.height / 2.0};
    if (std::isfinite(terrainScale) && terrainScale > 0.0) result.pixelsPerMapUnit = result.width / 184.0 / terrainScale;
    return result;
}
