#pragma once
#include "../Domain/MapData.h"
#include "FreeRoutePoint.h"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <unordered_map>

namespace AutoRoute {
inline constexpr std::size_t MaxTargets = 500;
inline bool IsSurfaceTarget(const ItemDatas& item) { return item.layer.floorId.empty(); }
// A stop is either an official point (identity = its point id) or a free point the player
// dropped on empty map space while drawing a route by hand. Only free points may be missing a
// type, because only they have no point in the upstream catalogue to take one from.
inline bool IsFreeStop(const ItemDatas& item) { return item.layer.stopKind == StopKind::Free; }
inline std::string Key(const ItemDatas& item) {
    return PointIdentityKey(item);
}
// The distinct point types a route visits, in first-seen order. Free points carry no type and
// contribute nothing: the route filter has no icon or filter row to switch on for them.
inline std::vector<std::string> Kinds(const std::vector<ItemDatas>& stops) {
    std::vector<std::string> kinds;
    for (const auto& stop : stops) {
        if (IsFreeStop(stop) || stop.nameId.empty()) continue;
        if (std::find(kinds.begin(), kinds.end(), stop.nameId) == kinds.end()) kinds.push_back(stop.nameId);
    }
    return kinds;
}
struct Start {
    int sceneId = 0;
    Coordinate roc;
    std::string source = "playerSnapshot";
    std::int64_t confirmedUnixMs = 0;
    std::uint64_t generation = 0;
    bool valid = false;
};
struct HandRouteNode { std::string id; ItemDatas point; };
struct HandRouteEdge {
    std::string from, to;
    bool operator==(const HandRouteEdge&) const = default;
};
struct HandRouteEditor {
    std::vector<HandRouteNode> nodes;
    std::vector<HandRouteEdge> edges;
    // Counters survive deletion, undo and reload; identities are never recycled.
    std::size_t nextNodeId = 1, nextFreeId = 1;
};
struct Plan {
    std::string id, name, profileId;
    int sceneId = 0;
    Start start;
    std::vector<ItemDatas> stops;
    std::optional<HandRouteEditor> handEditor;
    std::unordered_set<std::string> skipped;
    std::vector<std::string> skipHistory;
    // Whether this route was saved as a farming route: "刷怪采集" belongs to the route, not to
    // the session and not to a global setting, because a route built to sweep monsters and
    // herbs is a farming route every time it is loaded. The runtime mode still ends with the
    // navigation — this only decides what it is when the route starts again.
    bool farmMode = false;
    // Which system produced this route. It changes no behaviour — the drawing, the ordering and
    // the completion rules are the same for both — and exists so the route list can label a row
    // and so the two are stored in separate folders.
    bool handDrawn = false;
    FreePointCategory routeCategory = FreePointCategory::Daily;
    bool legacyHandDrawn = true;
    // A drawing that has not been saved yet. It is never written to disk; it exists so the renderer
    // can tell "the path I am still building" apart from "a planned route awaiting confirmation".
    bool handDraft = false;
    // Whether applying this route should narrow the map filter to the point types it visits.
    // Like the farming setting this belongs to the route: a route built for 叮叮咚 is a route
    // the player wants to see only 叮叮咚 on.
    bool filterByRoute = true;
    // Which collection the route is filed under. It is a field of the route rather than a folder,
    // because the store already reads every route file to describe it in the list — one more key
    // costs nothing, while a folder would make listing recursive and would have to be taught to
    // Load, Delete and the active pointer. `default` is reserved and always exists, so a route
    // file written before collections existed already belongs to the default collection.
    std::string collection = "default";
};
// Scope identities and remap every reference together, including imported route copies.
inline void ScopeFreePoints(Plan& plan, const std::string& routeId) {
    std::unordered_map<std::string,std::string> keys;
    for(auto& item : plan.stops) if(IsFreeStop(item)) {
        const auto before=Key(item); item.freeRouteId=routeId; keys[before]=Key(item);
    }
    if(plan.handEditor)for(auto& node:plan.handEditor->nodes)if(IsFreeStop(node.point))
        node.point.freeRouteId=routeId;
    std::unordered_set<std::string> skipped;
    for(const auto& key : plan.skipped) skipped.insert(keys.contains(key)?keys.at(key):key);
    plan.skipped=std::move(skipped);
    for(auto& key : plan.skipHistory) if(keys.contains(key)) key=keys.at(key);
}
struct SolveResult {
    std::vector<ItemDatas> stops;
    double initialLength = 0, planarLength = 0;
    bool cancelled = false;
};
inline std::vector<FreeRouteMarker> FreeMarkers(const Plan& plan,const std::unordered_set<std::string>& completed,int current,std::uint64_t revision,bool navigationOnly=false) {
    std::vector<FreeRouteMarker> markers;
    for(std::size_t i=0;i<plan.stops.size();++i){
        const auto& point=plan.stops[i];const auto key=Key(point);
        if(!IsFreeStop(point)||(navigationOnly&&(completed.contains(key)||plan.skipped.contains(key))))continue;
        auto markerPoint=point;markerPoint.freeDisplayOrder=static_cast<int>(i+1);markerPoint.isSaved=completed.contains(key);
        markers.push_back({markerPoint,static_cast<int>(i+1),static_cast<int>(i)==current,plan.profileId,plan.id,revision});
    }
    return markers;
}
struct DrawVisibility {
    std::string profileId, activeId, previewId;
    bool navigating = false;
    std::uint64_t orderRevision = 0;
    bool comparisonVisible = false;
    // Hand-drawn routes used to bypass this entirely, which meant a stale line could outlive
    // switching profile, stopping the navigation or leaving the map. They are ordinary routes
    // now, so there is one rule and no exception.
    bool Allows(const RouteDatas& route, bool minimap = false) const {
        if (route.profileId != profileId || route.routePlanId.empty()) return false;
        if (route.preview) return !minimap && route.routePlanId == previewId;
        return route.routePlanId == activeId && route.orderRevision == orderRevision &&
            (!route.previousTarget || comparisonVisible) && (!minimap || navigating);
    }
};

// The start is fixed, the end is free, and there is no return-to-start edge.
// IDs, not coordinates, identify targets. Invalid input is rejected as a whole.
// Cancellation returns no partial route; the caller must also fence stale results.
inline SolveResult Solve(const Start& start, std::vector<ItemDatas> targets,
    std::function<bool()> cancelled = {}) {
    const auto finite = [](const Coordinate& value) { return std::isfinite(value.x) && std::isfinite(value.y); };
    if (!start.valid || start.sceneId <= 0 || !finite(start.roc))
        throw std::invalid_argument("route-start-invalid");
    if (targets.size() > MaxTargets) throw std::invalid_argument("route-target-limit");
    std::unordered_set<std::string> identities;
    for (const auto& target : targets) {
        if (target.itemId.empty() || !finite(target.itemMapROC)) throw std::invalid_argument("route-target-invalid");
        if (!identities.insert(Key(target)).second) throw std::invalid_argument("route-target-duplicate");
    }
    const auto stopped = [&] { return cancelled && cancelled(); };
    const auto stopResult = [] { SolveResult result; result.cancelled = true; return result; };
    if (stopped()) return stopResult();
    std::sort(targets.begin(), targets.end(), [](const auto& a, const auto& b) { return Key(a) < Key(b); });
    const auto count = targets.size(), stride = count + 1;
    // Store start at index count; precomputation bounds the work in each 2-opt pass.
    std::vector<double> distances(stride * stride, 0.0);
    const auto position = [&](std::size_t index) { return index == count ? start.roc : targets[index].itemMapROC; };
    const auto distance = [&](std::size_t a, std::size_t b) { return distances[a * stride + b]; };
    for (std::size_t i = 0; i < stride; ++i) {
        if (stopped()) return stopResult();
        for (std::size_t j = i + 1; j < stride; ++j) {
            const auto a = position(i), b = position(j);
            const double value = std::hypot(a.x - b.x, a.y - b.y);
            if (!std::isfinite(value)) throw std::invalid_argument("route-distance-invalid");
            distances[i * stride + j] = distances[j * stride + i] = value;
        }
    }
    std::vector<std::size_t> order;
    std::vector<bool> used(count, false);
    std::size_t current = count;
    for (std::size_t step = 0; step < count; ++step) {
        if (stopped()) return stopResult();
        std::size_t next = count;
        for (std::size_t candidate = 0; candidate < count; ++candidate)
            if (!used[candidate] && (next == count || distance(current, candidate) < distance(current, next))) next = candidate;
        used[next] = true; order.push_back(next); current = next;
    }
    const auto length = [&] {
        double total = 0;
        std::size_t previous = count;
        for (const auto index : order) { total += distance(previous, index); previous = index; }
        if (!std::isfinite(total)) throw std::invalid_argument("route-length-invalid");
        return total;
    };
    SolveResult result;
    result.initialLength = length();
    for (int pass = 0; pass < 20; ++pass) {
        bool improved = false;
        for (std::size_t i = 0; i + 1 < count; ++i) {
            if (stopped()) return stopResult();
            for (std::size_t j = i + 1; j < count; ++j) {
                const auto previous = i == 0 ? count : order[i - 1];
                double before = distance(previous, order[i]);
                double after = distance(previous, order[j]);
                if (j + 1 < count) {
                    before += distance(order[j], order[j + 1]);
                    after += distance(order[i], order[j + 1]);
                }
                const double tolerance = 1e-10 * (std::max)({1.0, before, after});
                if (after + tolerance < before) {
                    std::reverse(order.begin() + i, order.begin() + j + 1);
                    improved = true;
                }
            }
        }
        if (!improved) break;
    }
    if (stopped()) return stopResult();
    result.planarLength = length();
    result.stops.reserve(count);
    for (const auto index : order) result.stops.push_back(std::move(targets[index]));
    return result;
}
} // namespace AutoRoute
