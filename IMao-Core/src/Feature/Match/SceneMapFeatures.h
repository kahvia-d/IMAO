#pragma once

#include "../RuntimeFeatureRepository.h"
#include "UniqueFeatureSubset.h"
#include "../../Coordinate/CoordinateStruct.h"
#include <cmath>

// Legacy callers used an X/Y-only window over all scenes. Independent scene
// coordinates overlap, so even OCR confirmation must select indexed rows first.
inline ImageFeatureData SceneMapFeaturesNear(const RuntimeFeatureResources& resources,
    int sceneId, const Coordinate& center, double radius) {
    ImageFeatureData result;
    if (!resources.visualIndexReady || !Scene::IsRuntimeApproved(sceneId) ||
        !std::isfinite(radius) || radius <= 0 || !std::isfinite(center.x) || !std::isfinite(center.y)) return result;
    std::vector<unsigned char> selected(resources.map.imgKeypoints.size(), 0);
    for (std::size_t i = 0; i < resources.visualIndex.tiles.size(); ++i) {
        const auto& tile = resources.visualIndex.tiles[i];
        if ((i < resources.baseVisualTileCount ? 1 : tile.sceneId) != sceneId) continue;
        resources.VisitTileRows(i, [&](std::uint32_t row) {
            if (row >= selected.size() || row >= static_cast<std::size_t>(resources.map.imgDescriptors.rows)) return;
            const auto& point = resources.map.imgKeypoints[row].pt;
            if (std::abs(point.x - center.x) <= radius && std::abs(point.y - center.y) <= radius) selected[row] = 1;
        });
    }
    std::vector<uint32_t> rows;
    for (std::size_t row = 0; row < selected.size(); ++row)
        if (selected[row] && resources.FeatureRowEnabled(row)) rows.push_back(static_cast<uint32_t>(row));
    return UniqueFeatureSubset(resources.map, rows);
}
