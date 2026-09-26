#pragma once
#include "../CoordinateStruct.h"

inline constexpr double kNominalMinimapTerrainScale = 194.0 / 184.0;

struct MinimapProjectionGeometry {
    Coordinate center;
    double pixelsPerMapUnit = 0.0;
    int left = 0, top = 0, width = 0, height = 0;
};

// Use exactly the integer crop the 184px minimap localizer is fed, so a marker drawn from this
// geometry lands on the pixel the localizer measured. The crop is square because the client's single
// HUD scale is applied to both axes (HudLayout.h): a 2560x1600 client crops 246x246, not 246x274.
inline MinimapProjectionGeometry GetMinimapProjectionGeometry(const RECT& rect,
    double terrainScale = kNominalMinimapTerrainScale) {
    const cv::Rect area = hud::MapBox(hud::Layout::For(static_cast<double>(rect.right - rect.left),
        static_cast<double>(rect.bottom - rect.top)), hud::kMinimap);
    MinimapProjectionGeometry result;
    result.left = area.x;
    result.top = area.y;
    result.width = area.width;
    result.height = area.height;
    result.center = {result.left + result.width / 2.0, result.top + result.height / 2.0};
    if (std::isfinite(terrainScale) && terrainScale > 0.0) result.pixelsPerMapUnit = result.width / 184.0 / terrainScale;
    return result;
}
