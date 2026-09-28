#pragma once
#include "RoutePlanningModel.h"
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace AutoRoute {
// Drawing a route by hand: press the key, click a point on the big map, repeat. Unlike the old
// tool — which stored pairs of raw coordinates and committed a segment on every second press —
// this collects an ordered list of real stops, so a hand-drawn route is the same kind of thing as
// a planned one and inherits the list, switching, completion and farming behaviour for free.
//
// A stop is either a point that already exists on the map (the player connected to it) or a free
// point the player dropped on empty space. The latter has no type and therefore no icon, which is
// why it is drawn as a numbered dot instead.
//
// Two ids must not be confused here: `scene` is the runtime scene the route belongs to (what a
// route file and the map filter use), while `stateId` is Kuro's top-level state that identifies
// which point catalogue the route's points come from. A route stores the scene and its points
// carry the state id; they are different numbers for the same place.
class HandDrawnDraft {
public:
    bool Active() const { return active; }
    // A drawing that has been finished but not yet saved. Holding points without being active is a
    // real state: leaving the drawing (Escape) must keep what was drawn so the player can go and
    // save it, and the toolbar cannot be opened while the drawing still owns the map.
    bool Pending() const { return !active && !points.empty(); }
    int SceneId() const { return scene; }
    const std::vector<ItemDatas>& Points() const { return points; }
    std::size_t Size() const { return points.size(); }

    // Begins a drawing on one map. A session belongs to a single map because a route names one map:
    // the start, the projection and the map filter all key off it.
    //
    // Calling this while a finished drawing is still waiting to be saved resumes that drawing rather
    // than starting an empty one. Resuming is what "keep drawing" means, and wiping the points here
    // would throw away work the player explicitly asked to come back to.
    void Start(int sceneId, int stateId) {
        if (!Scene::IsKnown(sceneId)) throw std::invalid_argument("请先在游戏大地图上打开要绘制的区域");
        if (stateId <= 0) throw std::invalid_argument("当前地图没有点位数据，无法手绘");
        if (Pending()) {
            if (scene != sceneId)
                throw std::invalid_argument("已有一条未保存的手绘路线属于别的地图，请先在路线列表里保存或放弃它");
            state = stateId;
            active = true;
            return;
        }
        active = true;
        scene = sceneId;
        state = stateId;
        points.clear();
        freeCount = 0;
    }

    // Adds the next stop in drawing order. Returns what it stored, so the caller can report the
    // point's name or its number.
    ItemDatas Add(const ItemDatas& point) {
        if (!active) throw std::runtime_error("请先开始手绘路线");
        if (point.layer.stateId != state)
            throw std::invalid_argument("手绘路线只能在同一张地图上，请先完成或放弃当前路线");
        if (points.size() >= MaxTargets) throw std::invalid_argument("单条路线最多 500 点");
        ItemDatas stop = point;
        if (IsFreeStop(point) || point.itemId.empty()) {
            if (!std::isfinite(point.itemMapROC.x) || !std::isfinite(point.itemMapROC.y))
                throw std::invalid_argument("手绘点位坐标无效");
            // A free point is invented here, so its identity and its lack of a type are both set
            // here; everything downstream only ever reads them.
            stop = ItemDatas{};
            stop.itemId = "free:" + std::to_string(++freeCount);
            stop.itemMapROC = point.itemMapROC;
            stop.layer.stateId = state;
            stop.layer.stopKind = StopKind::Free;
        }
        points.push_back(std::move(stop));
        return points.back();
    }

    // Removes the most recently added stop.
    bool Undo() {
        // A finished drawing can still be edited until it is saved or discarded, so this asks about
        // the points rather than about the drawing being in progress.
        if (points.empty()) return false;
        points.pop_back();
        // Free numbering restarts from what is left, so the visible badges stay 1..n with no gap.
        std::size_t remaining = 0;
        for (const auto& point : points) if (IsFreeStop(point)) ++remaining;
        freeCount = remaining;
        return true;
    }

    // Stops drawing but keeps what was drawn, so it can still be saved. This is what leaving the
    // drawing means: the player cannot reach the toolbar while the drawing is still taking clicks.
    bool Finish() {
        if (!active) return false;
        active = false;
        return !points.empty();
    }

    // Throws the drawing away, including one that was finished but not saved yet.
    void Cancel() {
        active = false;
        scene = 0;
        state = 0;
        points.clear();
        freeCount = 0;
    }

    // A finished drawing. The first stop is the start and the last is the end: the player drew an
    // ordered path, and reordering it would be "planning", which is the other feature.
    Plan Commit(const std::string& id, const std::string& name, const std::string& profileId) const {
        if (points.empty()) throw std::runtime_error("当前没有正在绘制的手绘路线");
        if (points.size() < 2) throw std::runtime_error("手绘路线至少需要两个点");
        Plan plan;
        plan.id = id;
        plan.name = name;
        plan.profileId = profileId;
        plan.sceneId = scene;
        plan.handDrawn = true;
        plan.start.sceneId = scene;
        plan.start.roc = points.front().itemMapROC;
        plan.start.source = "manual";
        plan.start.valid = true;
        plan.stops = points;
        return plan;
    }

private:
    bool active = false;
    int scene = 0;
    int state = 0;
    std::size_t freeCount = 0;
    std::vector<ItemDatas> points;
};
} // namespace AutoRoute
