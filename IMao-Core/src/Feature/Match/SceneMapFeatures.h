#pragma once

#include "../RuntimeFeatureRepository.h"
#include "UniqueMapFeatures.h"
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
        const std::size_t end = static_cast<std::size_t>(tile.featureRowOffset) + tile.featureRowCount;
        if (end > resources.visualIndex.featureRows.size()) continue;
        for (std::size_t j = tile.featureRowOffset; j < end; ++j) {
            const auto row = resources.visualIndex.featureRows[j];
            if (row >= selected.size() || row >= static_cast<std::size_t>(resources.map.imgDescriptors.rows)) continue;
            const auto& point = resources.map.imgKeypoints[row].pt;
            if (std::abs(point.x - center.x) <= radius && std::abs(point.y - center.y) <= radius) selected[row] = 1;
        }
    }
    UniqueMapFeatures unique;
    for (std::size_t row = 0; row < selected.size(); ++row) {
        if (!selected[row] || !resources.FeatureRowEnabled(row)) continue;
        const auto descriptor = resources.map.imgDescriptors.row(static_cast<int>(row));
        if (!unique.Insert(resources.map.imgKeypoints[row], descriptor)) continue;
        result.imgKeypoints.push_back(resources.map.imgKeypoints[row]);
        result.imgDescriptors.push_back(descriptor);
    }
    return result;
}
