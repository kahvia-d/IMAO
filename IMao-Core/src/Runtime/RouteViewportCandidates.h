#pragma once
#include "RoutePlanningModel.h"
#include <cmath>
#include <unordered_set>

namespace AutoRoute {
// Bulk viewport selection follows the map client area, even behind the tools
// window. Direct canvas gestures may only target unobscured points.
struct ViewportCandidates {
    std::vector<ItemDatas> inViewport;
    std::vector<ItemDatas> onCanvas;
    std::vector<Coordinate> canvasPositions;

    void Add(const ItemDatas& item, Coordinate position, double width, double height,
        bool completed, bool coveredByTools) {
        if (completed || !std::isfinite(position.x) || !std::isfinite(position.y) ||
            position.x < 0 || position.y < 0 || position.x > width || position.y > height ||
            !keys.insert(Key(item)).second) return;
        inViewport.push_back(item);
        if (!coveredByTools) {
            onCanvas.push_back(item);
            canvasPositions.push_back(position);
        }
    }
private:
    std::unordered_set<std::string> keys;
};
}
