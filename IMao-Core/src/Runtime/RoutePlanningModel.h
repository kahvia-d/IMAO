#pragma once
#include "../Domain/MapData.h"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <numeric>
#include <stdexcept>
#include <unordered_set>

namespace AutoRoute {
inline constexpr std::size_t MaxTargets = 500;
inline std::string Key(const ItemDatas& item) {
    return std::to_string(item.layer.stateId) + ":" + item.itemId;
}
struct Start {
    int sceneId = 0;
    Coordinate roc;
    std::string source = "playerSnapshot";
    std::int64_t confirmedUnixMs = 0;
    std::uint64_t generation = 0;
    bool valid = false;
};
struct Plan {
    std::string id, name, profileId;
    int sceneId = 0;
    Start start;
    std::vector<ItemDatas> stops;
    std::unordered_set<std::string> skipped;
    std::vector<std::string> skipHistory;
};
struct SolveResult {
    std::vector<ItemDatas> stops;
    double initialLength = 0, planarLength = 0;
    bool cancelled = false;
};
struct DrawVisibility {
    std::string profileId, activeId, previewId;
    bool navigating = false;
    bool Allows(const RouteDatas& route, bool minimap = false) const {
        if (!route.automatic) return true;
        if (route.profileId != profileId || route.routePlanId.empty()) return false;
        if (route.preview) return !minimap && route.routePlanId == previewId;
        return route.routePlanId == activeId && (!minimap || navigating);
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
