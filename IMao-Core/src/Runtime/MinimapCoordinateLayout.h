#pragma once
#include "FrameState.h"
#include "../Coordinate/locationCalculator/MinimapProjectionGeometry.h"

inline void AuthorizeReadoutMinimapPublication(OverlayFrame& frame,
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
    const auto& proof = frame.minimapReadout;
    if (!proof.active) return; // SURF retains its own publication policy.
    auto& pose = frame.minimapMotion;
    pose.reliable = false;
    // A background visual search can be recovering while the committed readout
    // remains valid. Authorize this layout from that fix, not the shared search state.
    if (!frame.Drawable(now) || !frame.focused || !frame.minimapVisible || frame.mapVisible ||
        frame.playerScene <= 0 || proof.scene != frame.playerScene || proof.scene != pose.scene ||
        proof.generation != pose.generation || proof.fixFrameId == 0 || proof.fixFrameId > frame.frameId ||
        proof.capturedAt == std::chrono::steady_clock::time_point{} || proof.capturedAt > frame.capturedAt ||
        now < proof.capturedAt ||
        !std::isfinite(pose.center.x) || !std::isfinite(pose.center.y) ||
        !std::isfinite(proof.terrainScale) || proof.terrainScale <= 0.0 ||
        !std::isfinite(pose.pixelsPerUnit) || pose.pixelsPerUnit <= 0.0 ||
        !std::isfinite(pose.screenCenter.x) || !std::isfinite(pose.screenCenter.y)) return;
    const auto geometry = GetMinimapProjectionGeometry(frame.clientRect, proof.terrainScale);
    if (geometry.width <= 0 || geometry.height <= 0 ||
        std::hypot(geometry.center.x - pose.screenCenter.x, geometry.center.y - pose.screenCenter.y) > 0.01 ||
        std::abs(geometry.pixelsPerMapUnit - pose.pixelsPerUnit) > 1e-6) return;
    pose.reliable = true;
}

inline void ProjectMinimapLayout(OverlayFrame& frame, const Coordinate& acceptedPlayerROC) {
    const auto& pose = frame.minimapMotion;
    const auto project = [&](const Coordinate& point) {
        return Coordinate{pose.screenCenter.x + (point.x - acceptedPlayerROC.x) * pose.pixelsPerUnit,
            pose.screenCenter.y - (point.y - acceptedPlayerROC.y) * pose.pixelsPerUnit};
    };
    // The nearby catalogue can contain coordinates from another center. Reproject
    // its immutable payload here so every layer describes the accepted fix.
    frame.minimapMarkers.center = pose.screenCenter;
    for (auto& marker : frame.minimapMarkers.markers) marker.screenCoordiante = project(marker.itemMapROC);
    for (auto& marker : frame.minimapFreeMarkers) marker.point.screenCoordiante = project(marker.point.itemMapROC);
    for (auto& route : frame.minimapRoutes) {
        route.routePointsScreenCoord.clear();
        for (const auto& point : route.routePointsROC) route.routePointsScreenCoord.push_back(project(point));
    }
}
