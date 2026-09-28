#pragma once
#include "RoutePlanningModel.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace AutoRoute {
// Hand-drawn routes from before this feature were stored as a bare list of line segments per
// scene: {"World": [[[x,y],[x,y]], ...]}, in the same scene-relative ROC space the picker
// produces. There is no point identity in that format, so an import has to invent one — but it
// must never invent a *type*, because a type drives the icon and the map filter.
//
// A loaded legacy route is therefore a normal plan whose stops are free points, except where an
// endpoint lands exactly on a known point of the scene: that one is upgraded to the real point
// so the route keeps the type and icon the player was drawing around.
class LegacyDocument {
public:
    // A scene name that resolves to one of the runtime scenes; the only scene lookup the
    // importer needs, kept as a parameter so the import can be tested without any map data.
    using SceneLookup = std::function<int(const std::string&)>;
    // Reports the official point at a position, if the scene has one there. Coordinates are the
    // scene-relative ROC the route stores.
    using PointLookup = std::function<std::optional<ItemDatas>(int, const Coordinate&)>;

    struct Result {
        std::vector<Plan> plans;
        // Why a document could not be imported at all. Empty means it was imported, even if the
        // result holds no plan because the file held no segment.
        std::string rejected;
    };

    // `endpointTolerance` is intentionally tiny: upgrading an endpoint to an official point
    // changes what the route is *about*, so a near miss must stay a free point rather than
    // silently claim the player meant the collectible standing next to their line.
    static Result Import(const nlohmann::json& document, const std::string& name, const std::string& profileId,
        const SceneLookup& sceneLookup, const PointLookup& pointLookup, double endpointTolerance = 1e-6) {
        Result result;
        if (!document.is_object()) { result.rejected = "路线文件必须是场景对象"; return result; }
        for (const auto& [sceneName, segments] : document.items()) {
            const int sceneId = sceneLookup(sceneName);
            if (sceneId <= 0) { result.rejected = "路线场景无法识别：" + sceneName; return result; }
            if (!segments.is_array()) { result.rejected = "路线线段必须是数组"; return result; }
            std::vector<Coordinate> points;
            for (const auto& segment : segments) {
                if (!segment.is_array() || segment.size() != 2) { result.rejected = "路线线段必须包含两个端点"; return result; }
                for (std::size_t endpoint = 0; endpoint < 2; ++endpoint) {
                    const auto& value = segment[endpoint];
                    if (!value.is_array() || value.size() != 2 || !value[0].is_number() || !value[1].is_number()) {
                        result.rejected = "路线端点必须包含两个数值坐标";
                        return result;
                    }
                    const Coordinate point{value[0].get<double>(), value[1].get<double>()};
                    if (!std::isfinite(point.x) || !std::isfinite(point.y)) { result.rejected = "路线坐标必须为有限数值"; return result; }
                    points.push_back(point);
                }
            }
            if (points.empty()) continue;
            Plan plan;
            plan.name = name;
            plan.profileId = profileId;
            plan.sceneId = sceneId;
            plan.handDrawn = true;
            plan.filterByRoute = true;
            plan.start = Start{sceneId, points.front(), "legacyImport", 0, 0, true};
            const auto* scene = Scene::Find(sceneId);
            std::size_t number = 0;
            for (const auto& point : points) {
                ++number;
                // Every endpoint is kept, including the shared joints between consecutive
                // segments: the file records mouse picks, and two picks that merely *should* be
                // the same place are not the same place — fusing them would move the line.
                if (const auto official = pointLookup ? pointLookup(sceneId, point) : std::nullopt) {
                    if (std::abs(official->itemMapROC.x - point.x) <= endpointTolerance &&
                        std::abs(official->itemMapROC.y - point.y) <= endpointTolerance &&
                        official->layer.stateId == (scene ? scene->kuroStateId : 0)) {
                        plan.stops.push_back(*official);
                        continue;
                    }
                }
                ItemDatas free;
                free.itemId = "free:" + std::to_string(number);
                free.itemMapROC = point;
                free.layer.stateId = scene ? scene->kuroStateId : 0;
                free.layer.stopKind = StopKind::Free;
                plan.stops.push_back(std::move(free));
            }
            result.plans.push_back(std::move(plan));
        }
        return result;
    }
};
} // namespace AutoRoute
