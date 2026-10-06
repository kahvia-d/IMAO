#pragma once
#include "HandRouteEditor.h"
#include <deque>

namespace AutoRoute {
inline bool HandDrawingInputAllowed(bool active,int draftScene,int displayedScene,bool focused,bool fresh) {
    return active&&draftScene>0&&draftScene==displayedScene&&focused&&fresh;
}
// One editor is shared by new and saved hand routes. Nodes are independent of
// their navigation order; only explicit edges produce navigation stops.
class HandDrawnDraft {
public:
    bool CategoryLocked() const { return categoryLocked; }
    bool Active() const { return active; }
    bool Pending() const { return !active&&(categoryLocked||!editor.nodes.empty()); }
    int SceneId() const { return scene; }
    const std::vector<ItemDatas>& Points() const { return points; }
    const HandRouteEditor& Editor() const { return editor; }
    std::size_t Size() const { return editor.nodes.size(); }
    const std::string& RouteId() const { return routeId; }
    FreePointCategory Category() const { return category; }
    FreePointIcon Icon() const { return icon; }
    bool EditingSaved() const { return original.has_value(); }
    const std::optional<Plan>& Original() const { return original; }
    std::vector<ItemDatas> OrderedPoints() const { return HandRouteStops(editor); }
    bool CanUndo() const { return !history.empty(); }
    bool CanCommit() const { return OrderedPoints().size()>=2; }
    std::string NodeForKey(const std::string& key) const {
        for(const auto& node:editor.nodes)if(Key(node.point)==key)return node.id;
        return {};
    }
    void SelectIcon(FreePointIcon value) {
        RequireActive();
        if(category==FreePointCategory::Collectible&&value!=FreePointIcon::Number)
            throw std::invalid_argument("收集物路线只能使用数字图标");
        icon=value;
    }
    void Start(int sceneId,int stateId,FreePointCategory value=FreePointCategory::Daily,std::string id={}) {
        if(!Scene::IsKnown(sceneId))throw std::invalid_argument("请先在游戏大地图上打开要绘制的区域");
        if(stateId<=0)throw std::invalid_argument("当前地图没有点位数据，无法手绘");
        if((active||Pending())&&scene!=sceneId)
            throw std::invalid_argument("请先保存或放弃另一张地图的手绘路线");
        if(active&&categoryLocked){if(value!=category)throw std::invalid_argument("已经开始加点，路线类型不能更改");return;}
        if(Pending()){active=true;return;}
        Cancel();active=true;scene=sceneId;state=stateId;category=value;routeId=std::move(id);
    }
    void Edit(const Plan& plan) {
        if(active||Pending())throw std::invalid_argument("请先保存或放弃当前手绘草稿");
        if(!plan.handDrawn)throw std::invalid_argument("只能修改手绘路线");
        auto next=EditorFromPlan(plan);HandRouteOrder(next);
        Cancel();editor=std::move(next);original=plan;scene=plan.sceneId;
        state=Scene::Find(scene)->kuroStateId;routeId=plan.id;category=plan.routeCategory;
        active=true;categoryLocked=true;RefreshPoints();
    }
    ItemDatas Add(const ItemDatas& point) {
        RequireActive();
        if(point.layer.stateId!=state)throw std::invalid_argument("手绘路线只能在同一张地图上");
        if(!IsFreeStop(point)&&!point.itemId.empty())
            for(const auto& node:editor.nodes)if(Key(node.point)==Key(point))return node.point;
        if(Size()>=MaxTargets)throw std::invalid_argument("单条路线最多 500 点");
        if(!std::isfinite(point.itemMapROC.x)||!std::isfinite(point.itemMapROC.y))throw std::invalid_argument("手绘点位坐标无效");
        auto stop=point;
        if(IsFreeStop(point)||point.itemId.empty()) {
            stop=ItemDatas{};stop.itemId="free:"+std::to_string(editor.nextFreeId++);
            stop.itemMapROC=point.itemMapROC;stop.layer.stateId=state;stop.layer.stopKind=StopKind::Free;
            stop.freeRouteId=routeId;stop.freeCategory=category;stop.freeIcon=icon;
        }
        Remember();editor.nodes.push_back({"n"+std::to_string(editor.nextNodeId++),stop});
        categoryLocked=true;RefreshPoints();return stop;
    }
    void Connect(const std::string& from,const std::string& to) {
        RequireActive();
        if(from==to||!FindHandNode(editor,from)||!FindHandNode(editor,to))throw std::invalid_argument("请选择两个不同的路线点位");
        auto next=editor;
        std::erase_if(next.edges,[&](const auto& edge){return edge.from==from||edge.to==to||(edge.from==to&&edge.to==from);});
        next.edges.push_back({from,to});HandRouteOrder(next);
        if(next.edges==editor.edges)return;
        Remember();editor=std::move(next);
    }
    void Remove(const std::string& id) {
        RequireActive();if(!FindHandNode(editor,id))throw std::invalid_argument("点位已不在编辑路线中");
        auto next=editor;std::string before,after;
        for(const auto& edge:next.edges){if(edge.to==id)before=edge.from;if(edge.from==id)after=edge.to;}
        std::erase_if(next.edges,[&](const auto& e){return e.from==id||e.to==id;});
        std::erase_if(next.nodes,[&](const auto& n){return n.id==id;});
        if(!before.empty()&&!after.empty())next.edges.push_back({before,after});
        HandRouteOrder(next);Remember();editor=std::move(next);RefreshPoints();
    }
    void ConnectPoint(const std::string& from,const ItemDatas& point) {
        auto next=*this;next.history.clear();
        const auto stored=next.Add(point);next.Connect(from,next.NodeForKey(Key(stored)));
        Remember();editor=std::move(next.editor);categoryLocked=true;RefreshPoints();
    }
    bool Undo() {
        if(history.empty())return false;
        const auto nodeCounter=editor.nextNodeId,freeCounter=editor.nextFreeId;
        editor=std::move(history.back());history.pop_back();
        editor.nextNodeId=std::max(editor.nextNodeId,nodeCounter);editor.nextFreeId=std::max(editor.nextFreeId,freeCounter);
        RefreshPoints();return true;
    }
    bool Finish() { if(!active)return false;active=false;return Pending(); }
    void Cancel() {
        active=false;categoryLocked=false;scene=state=0;routeId.clear();
        editor={};points.clear();history.clear();original.reset();icon=FreePointIcon::Number;
    }
    Plan Commit(const std::string& id,const std::string& name,const std::string& profile) const {
        const auto ordered=OrderedPoints();
        if(ordered.size()<2)throw std::runtime_error("请连接成唯一一条至少两个点的路线后再保存");
        Plan plan=original.value_or(Plan{});
        plan.id=id;plan.name=EditingSaved()?original->name:name;plan.profileId=profile;plan.sceneId=scene;
        plan.handDrawn=true;plan.routeCategory=category;
        if(!EditingSaved()){plan.legacyHandDrawn=false;plan.collection.clear();}
        plan.start={scene,ordered.front().itemMapROC,"manual",0,0,true};plan.stops=ordered;plan.handEditor=editor;
        std::unordered_set<std::string> surviving;for(const auto& stop:plan.stops)surviving.insert(Key(stop));
        std::erase_if(plan.skipped,[&](const auto& key){return !surviving.contains(key);});
        std::erase_if(plan.skipHistory,[&](const auto& key){return !plan.skipped.contains(key);});
        ScopeFreePoints(plan,id);return plan;
    }
private:
    bool active=false,categoryLocked=false;
    int scene=0,state=0;
    std::string routeId;
    FreePointCategory category=FreePointCategory::Daily;
    FreePointIcon icon=FreePointIcon::Number;
    HandRouteEditor editor;
    std::vector<ItemDatas> points;
    std::deque<HandRouteEditor> history;
    std::optional<Plan> original;
    void RequireActive() const { if(!active)throw std::runtime_error("请先开始手绘路线"); }
    void Remember(){if(history.size()==100)history.pop_front();history.push_back(editor);}
    void RefreshPoints(){points.clear();for(const auto& node:editor.nodes)points.push_back(node.point);}
};
} // namespace AutoRoute
