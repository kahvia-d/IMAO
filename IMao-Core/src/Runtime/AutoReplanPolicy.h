#pragma once
#include "RoutePlanningModel.h"
#include "OverlayMotion.h"
#include <chrono>
#include <limits>
#include <optional>

namespace AutoRoute {
using ReplanClock = std::chrono::steady_clock;
inline constexpr auto ReplanPeriod = std::chrono::milliseconds(2000);
inline constexpr auto TargetSwitchCooldown = std::chrono::milliseconds(8000);
inline constexpr auto PlayerFixLifetime = std::chrono::milliseconds(750);
inline constexpr auto CaptureLifetime = std::chrono::milliseconds(250);
inline constexpr double NearbyPixels = 10.0;

// Empty result permits the switch; nonempty results are user-visible wait states.
inline std::string CheckTargetSwitch(const std::string& next, bool proximityValid, double distance,
    ReplanClock::time_point now, ReplanClock::time_point lastSwitch,
    std::string& candidate, ReplanClock::time_point& since) {
    if(!proximityValid || !std::isfinite(distance) || distance<0){candidate.clear();since={};return "waitingForLocation";}
    if(distance<NearbyPixels){candidate.clear();since={};return "nearTarget";}
    if(lastSwitch!=ReplanClock::time_point{}&&(now<lastSwitch||now-lastSwitch<TargetSwitchCooldown))return "cooldown";
    if(candidate!=next||now<since){candidate=next;since=now;return "confirmingTarget";}
    return now-since<ReplanPeriod ? "confirmingTarget" : "";
}

struct PlayerObservation {
    std::string profileId;
    std::uint64_t sessionId = 0, continuityGeneration = 0, fixSequence = 0;
    int sceneId = 0;
    Coordinate roc;
    ReplanClock::time_point fixCapturedAt{}, latestCaptureAt{};
    bool visual = false, valid = false;
    bool Fresh(ReplanClock::time_point now) const {
        return valid && visual && sessionId && fixSequence && sceneId > 0 &&
            std::isfinite(roc.x) && std::isfinite(roc.y) && now >= fixCapturedAt && now >= latestCaptureAt &&
            now - fixCapturedAt <= PlayerFixLifetime && now - latestCaptureAt <= CaptureLifetime;
    }
};
struct ProximityObservation {
    std::string profileId, routeId, targetKey;
    std::uint64_t sessionId = 0, orderRevision = 0, sourceFrameId = 0;
    int sceneId = 0;
    ReplanClock::time_point capturedAt{}, presentedAt{};
    double distancePixels = std::numeric_limits<double>::infinity();
    bool valid = false;
};
struct StablePlayer {
    PlayerObservation last;
    ReplanClock::time_point since{};
    unsigned count = 0;
    void Reset() { last = {}; since = {}; count = 0; }
    void Observe(const PlayerObservation& p, ReplanClock::time_point now) {
        if (!p.Fresh(now)) { Reset(); return; }
        const bool same = count && last.profileId == p.profileId && last.sessionId == p.sessionId &&
            last.sceneId == p.sceneId && last.continuityGeneration == p.continuityGeneration &&
            p.fixSequence >= last.fixSequence && p.fixCapturedAt >= last.fixCapturedAt &&
            p.fixCapturedAt - last.fixCapturedAt <= CaptureLifetime;
        if (!same) { count = 1; since = p.fixCapturedAt; }
        else if (last.fixSequence != p.fixSequence) ++count;
        last = p;
    }
    bool Ready(ReplanClock::time_point now) const {
        return count >= 3 && last.Fresh(now) && last.fixCapturedAt - since >= std::chrono::milliseconds(200);
    }
};
struct NearbyConfirmation {
    std::uint64_t frameId = 0;
    ReplanClock::time_point since{}, last{};
    bool nearby = false;
    void Reset() { *this = {}; }
    bool Observe(const ProximityObservation& p, ReplanClock::time_point now) {
        if (!p.valid || !p.sourceFrameId || !std::isfinite(p.distancePixels) || p.distancePixels < 0 ||
            now < p.capturedAt || now < p.presentedAt || now - p.capturedAt > CaptureLifetime ||
            now - p.presentedAt >= std::chrono::milliseconds(100) || p.distancePixels >= NearbyPixels) {
            Reset(); return false;
        }
        if (p.sourceFrameId == frameId) return nearby && last - since >= std::chrono::milliseconds(500);
        if (!nearby || p.sourceFrameId < frameId || p.capturedAt <= last || p.capturedAt - last > CaptureLifetime)
            since = p.capturedAt;
        nearby = true; frameId = p.sourceFrameId; last = p.capturedAt;
        return last - since >= std::chrono::milliseconds(500);
    }
};
inline double TargetDistancePixels(Coordinate target, Coordinate player, Coordinate center,
    double pixelsPerUnit, const OverlayScreenTransform& motion) {
    if (!(pixelsPerUnit > 0) || !std::isfinite(pixelsPerUnit) || !(motion.scale > 0) ||
        !std::isfinite(motion.scale)) return std::numeric_limits<double>::infinity();
    const auto shown = motion.Apply({center.x + (target.x-player.x)*pixelsPerUnit,
        center.y - (target.y-player.y)*pixelsPerUnit});
    return std::hypot(shown.x-center.x, shown.y-center.y);
}
inline std::vector<ItemDatas> Remaining(const Plan& plan, const std::unordered_set<std::string>& completed) {
    std::vector<ItemDatas> result;
    for (const auto& p : plan.stops) if (!completed.contains(Key(p)) && !plan.skipped.contains(Key(p))) result.push_back(p);
    return result;
}
inline bool SameOrder(const std::vector<ItemDatas>& a, const std::vector<ItemDatas>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y) { return Key(x)==Key(y); });
}
inline double Length(Coordinate start, const std::vector<ItemDatas>& stops) {
    double result = 0;
    for (const auto& p : stops) { result += std::hypot(p.itemMapROC.x-start.x,p.itemMapROC.y-start.y); start=p.itemMapROC; }
    return result;
}
// Map-coordinate spacing, never an estimate of game metres. Ignore co-located IDs.
inline double TargetSpacing(const std::vector<ItemDatas>& stops) {
    std::vector<double> distances;
    for (const auto& a : stops) {
        double best = std::numeric_limits<double>::infinity();
        for (const auto& b : stops) { const double d=std::hypot(a.itemMapROC.x-b.itemMapROC.x,a.itemMapROC.y-b.itemMapROC.y);
            if (d>0 && std::isfinite(d)) best=(std::min)(best,d); }
        if (std::isfinite(best)) distances.push_back(best);
    }
    if (distances.empty()) return 0;
    std::sort(distances.begin(),distances.end());
    const auto n=distances.size(); return n%2 ? distances[n/2] : (distances[n/2-1]+distances[n/2])/2;
}
inline bool Worthwhile(Coordinate start, const std::vector<ItemDatas>& oldOrder,
    const std::vector<ItemDatas>& newOrder, double spacing) {
    const double before=Length(start,oldOrder), after=Length(start,newOrder);
    return !SameOrder(oldOrder,newOrder) && spacing>0 && std::isfinite(before) && std::isfinite(after) &&
        before-after >= (std::max)(before*.01,spacing*.10);
}
// Fill only unfinished slots; all IDs, skipped records and their undo order survive.
inline Plan MergeRemaining(const Plan& original, const std::unordered_set<std::string>& completed,
    const std::vector<ItemDatas>& order) {
    const auto old=Remaining(original,completed);
    std::unordered_set<std::string> expected, actual;
    for (const auto& p : old) expected.insert(Key(p));
    for (const auto& p : order) if (!actual.insert(Key(p)).second) throw std::invalid_argument("replan-duplicate-target");
    if (old.size()!=order.size() || expected!=actual) throw std::invalid_argument("replan-target-set-changed");
    auto result=original; std::size_t i=0;
    for (auto& p : result.stops) if (!completed.contains(Key(p)) && !result.skipped.contains(Key(p))) p=order[i++];
    return result;
}
} // namespace AutoRoute
