#pragma once
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class RouteMarkerMouseAction { Forward, Capture };

inline RouteMarkerMouseAction RouteMarkerMouseActionFor(bool planning, bool rightButton,
    std::string_view tool, std::string_view target, bool focused = true, bool freshPlanningMap = false) {
    const bool marker = target.starts_with("p:") || target.starts_with("g:");
    if (planning && !rightButton && (tool == "pan" || tool == "point") && marker && (focused || freshPlanningMap))
        return RouteMarkerMouseAction::Capture;
    return RouteMarkerMouseAction::Forward;
}

inline bool RouteMarkerCanCapturePublishedHit(bool regionsFresh, bool planningMapFresh,
    RouteMarkerMouseAction action) {
    // The rendered overlay remains visible during a brief render hitch. If its map evidence is
    // still fresh, let that visible marker own the click even when the hit-region timestamp has
    // crossed the short input-latency threshold.
    return action == RouteMarkerMouseAction::Capture && (regionsFresh || planningMapFresh);
}

inline bool RoutePointSelectionKeepsCanvas(bool planning, std::string_view tool) {
    return planning && tool == "point";
}

inline bool RouteMarkerClickDismissesTools(bool planning, bool gamepad, bool toolsSession) {
    return planning && !gamepad && toolsSession;
}

inline bool RouteGamepadSelectionHasFocus(bool fromToolsGamepad, bool routeGamepadFocused,
    bool toolsCanvasFocused) {
    return fromToolsGamepad ? toolsCanvasFocused : routeGamepadFocused;
}

inline bool IsRoutePointHit(std::string_view target) { return target.starts_with("p:"); }

inline std::string RouteSnapSelectionTarget(std::string_view hit) {
    if (hit.starts_with("p:")) return std::string(hit);
    if (hit.starts_with("g:")) return "p:" + std::string(hit.substr(2));
    return {};
}

enum class RoutePointAction { Toggle, Snap };

inline RoutePointAction RoutePointActionFor(std::string_view hit) {
    return RouteSnapSelectionTarget(hit).empty() ? RoutePointAction::Snap : RoutePointAction::Toggle;
}

struct RouteSnapPoint {
    std::string selectionTarget;
    double x = 0, y = 0;
};

inline std::optional<RouteSnapPoint> NearestRouteSnapPoint(const std::vector<RouteSnapPoint>& points,
    double x, double y) {
    std::optional<RouteSnapPoint> nearest;
    double distanceSquared = std::numeric_limits<double>::infinity();
    for (const auto& point : points) {
        const auto distance = std::pow(point.x - x, 2) + std::pow(point.y - y, 2);
        if (distance < distanceSquared) { nearest = point; distanceSquared = distance; }
    }
    return nearest;
}
