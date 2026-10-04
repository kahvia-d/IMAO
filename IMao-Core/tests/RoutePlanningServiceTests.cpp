#include "RoutePlanningServiceTestHost.h"
#include "Runtime/RoutePlanStore.h"
#include "Runtime/RouteCollections.h"
#include "Runtime/RouteBundle.h"
#include "Runtime/RouteViewportCandidates.h"
#include "Runtime/RouteToolbarNavigation.h"
#include "Runtime/OverlayPanelLayout.h"
#include <algorithm>
#include <iostream>
#include <limits>
#include <set>
#include <thread>

using namespace std::chrono_literals;
using Clock=AutoRoute::ReplanClock;
using Json=nlohmann::json;
namespace {
int failures=0;std::uint64_t sequence=0;
void VerifyRouteListShape();
void VerifyMapSuspension(const AutoRoute::Plan& original);
void Check(bool condition,const char* message){if(!condition){++failures;std::cerr<<"FAIL: "<<message<<'\n';}}
Json Command(Json command){const auto result=RoutePlanningService::Command(command);
    if(!result.value("accepted",false))throw std::runtime_error(result.value("message","command failed"));return result;}
void Complete(const ItemDatas& point,bool value){DrawItemBase::HandleMarkerCommand({{"stateId",point.layer.stateId},{"pointId",point.itemId},{"completed",value}});}
std::vector<std::string> Ids(const AutoRoute::Plan& plan){std::vector<std::string> result;for(const auto& p:plan.stops)result.push_back(AutoRoute::Key(p));std::sort(result.begin(),result.end());return result;}
void Observe(double x=20){
    const auto now=Clock::now();const auto seq=++sequence;
    RoutePlanningService::UpdatePlayer({1,{x,0},"playerSnapshot",0,1,true});
    RoutePlanningService::SetPlayerAvailable(true);
    RoutePlanningService::ObservePlayer({"local",1,1,seq,1,{x,0},now,now,true,true});
    const auto view=RoutePlanningService::View();
    if(view.active&&view.currentTargetIndex>=0){const auto& point=view.active->stops[view.currentTargetIndex];
        const auto distance=AutoRoute::TargetDistancePixels(point.itemMapROC,{x,0},{100,100},1,{});
        RoutePlanningService::ObserveProximity({"local",view.active->id,AutoRoute::Key(point),1,view.orderRevision,seq,1,now,now,distance,true});}
}
template<class Condition> bool Pump(Condition condition,std::chrono::milliseconds timeout,double x=20){
    const auto deadline=Clock::now()+timeout;
    do{Observe(x);if(condition())return true;std::this_thread::sleep_for(80ms);}while(Clock::now()<deadline);
    return condition();
}
AutoRoute::Plan Prepare(){
    const auto* scene=Scene::Find(1);Json points=Json::array();
    for(const auto& pair:std::vector<std::pair<std::string,double>>{{"first",100},{"done",400},{"skip",500},{"second",0}})
        points.push_back({{"id",pair.first},{"x",pair.second/scene->scale*100},{"y",0},{"stateId",scene->kuroStateId}});
    points.push_back({{"id","layered"},{"x",300/scene->scale*100},{"y",0},
        {"stateId",scene->kuroStateId},{"floorId","16"},{"level","-1/16"}});
    DrawItemBase::itemsJsonData_World=Json::array({{{"id","chest"},{"name","test"},{"location",points}}});
    RoutePlanningService::Initialize();Command({{"action","new"},{"sceneId",1}});
    Command({{"action","setStart"},{"sceneId",1},{"x",100},{"y",0}});
    std::vector<std::string> keys;for(const auto* id: {"first","done","skip","second"})keys.push_back(std::to_string(scene->kuroStateId)+":"+id);
    Command({{"action","add"},{"keys",keys}});Command({{"action","generate"}});
    const auto deadline=Clock::now()+3s;
    while(!RoutePlanningService::View().preview&&Clock::now()<deadline)std::this_thread::sleep_for(10ms);
    Command({{"action","activate"}});auto plan=*RoutePlanningService::View().active;
    std::vector<ItemDatas> ordered;for(const auto& key:keys)for(const auto& p:plan.stops)if(AutoRoute::Key(p)==key)ordered.push_back(p);
    plan.stops=ordered;plan.skipped={keys[2]};plan.skipHistory={keys[2]};
    AutoRoute::RoutePlanStore store(StructuredLogger::root/"SavedRoutes"/"Auto");store.Save(plan,true);
    Complete(plan.stops[1],true);Command({{"action","load"},{"routeId",plan.id}});Command({{"action","resume"}});
    return plan;
}

std::vector<std::string> SelectionKeys(const std::vector<ItemDatas>& points){
    std::vector<std::string> result;
    for(const auto& point:points)result.push_back(AutoRoute::Key(point));
    std::sort(result.begin(),result.end());
    return result;
}

void VerifyMapSuspension(const AutoRoute::Plan& original){
    RoutePlanningService::SetAutoReplanEnabled(false);
    Command({{"action","end"}});
    RoutePlanningService::ObserveMap(1,original.stops);
    Command({{"action","handStart"},{"sceneId",1},{"category","collectible"}});
    Command({{"action","handPoint"},{"x",100.0},{"y",0.0},{"key",AutoRoute::Key(original.stops.front())}});
    Command({{"action","handPoint"},{"x",1234.0},{"y",5678.0}});
    const auto before=RoutePlanningService::View();
    const auto points=before.handDraft->stops;
    const auto samePoints=[&](const RoutePlanningView& view){
        if(!view.handDraft||view.handDraft->stops.size()!=points.size())return false;
        for(std::size_t i=0;i<points.size();++i){const auto& p=view.handDraft->stops[i];
            if(AutoRoute::Key(p)!=AutoRoute::Key(points[i])||p.itemMapROC.x!=points[i].itemMapROC.x||
                p.itemMapROC.y!=points[i].itemMapROC.y||p.layer.stopKind!=points[i].layer.stopKind)return false;}
        return true;
    };
    for(int cycle=0;cycle<3;++cycle){
        const auto oldGeneration=RoutePlanningService::View().generation;
        RoutePlanningService::MapUnavailable();
        const auto paused=RoutePlanningService::View();
        Check(paused.handDrawnActive&&paused.handDrawnCount==2&&samePoints(paused),
            "temporary map unavailability preserves an active mixed hand draft and its exact ordered points");
        Check(paused.active.has_value()==before.active.has_value()&&
            (!before.active||AutoRoute::SameOrder(paused.active->stops,before.active->stops)),
            "temporary map unavailability preserves the active saved route");
        Check(!RoutePlanningService::Command({{"action","handPoint"},{"x",999.0},{"y",999.0}}).value("accepted",false),
            "a suspended map refuses hand points even when the caller omits context");
        RoutePlanningService::MapUnavailable();
        Check(samePoints(RoutePlanningService::View()),"repeated blank render frames cannot clear the draft");
        RoutePlanningService::ObserveMap(1,original.stops);
        Check(RoutePlanningService::View().handDrawnActive&&samePoints(RoutePlanningService::View()),
            "observing the original map resumes drawing without a handStart command");
        Check(!RoutePlanningService::Command({{"action","handPoint"},{"x",999.0},{"y",999.0},
            {"expectedGeneration",oldGeneration}}).value("accepted",false),
            "a click captured before suspension cannot be replayed after the map returns");
    }
    // Keep testing even on the old implementation after reporting the loss above.
    if(!RoutePlanningService::View().handDrawnActive){
        Command({{"action","handStart"},{"sceneId",1},{"category","collectible"}});
        Command({{"action","handPoint"},{"x",100.0},{"y",0.0},{"key",AutoRoute::Key(original.stops.front())}});
        Command({{"action","handPoint"},{"x",1234.0},{"y",5678.0}});
    }
    Command({{"action","handPoint"},{"x",1357.0},{"y",2468.0}});
    Check(RoutePlanningService::View().handDraft->stops.back().itemId=="free:2",
        "continuing after suspension retains free-point numbering");
    Command({{"action","handUndo"}});
    Check(samePoints(RoutePlanningService::View()),"undo after resuming returns to the exact original points");
    Command({{"action","handFinish"}});
    RoutePlanningService::MapUnavailable();
    Check(RoutePlanningService::View().handDrawnPending&&samePoints(RoutePlanningService::View()),
        "temporary map unavailability preserves a finished unsaved hand draft");
    RoutePlanningService::ObserveMap(1,original.stops);
    Command({{"action","handCommit"},{"name","Suspension regression"}});
    Check(!RoutePlanningService::View().handDraft,"a preserved draft can still be saved after returning");

    for(int count=0;count<=1;++count){
        Command({{"action","handStart"},{"sceneId",1},{"category","collectible"}});
        if(count)Command({{"action","handPoint"},{"x",1234.0},{"y",5678.0}});
        RoutePlanningService::MapUnavailable();
        Check(RoutePlanningService::View().handDrawnActive&&RoutePlanningService::View().handDrawnCount==count,
            "empty and single-point hand drawings survive a temporary interruption");
        RoutePlanningService::ObserveMap(1,original.stops);
        Command({{"action","handCancel"}});
    }

    Command({{"action","handStart"},{"sceneId",1},{"category","collectible"}});
    Command({{"action","handPoint"},{"x",1234.0},{"y",5678.0}});
    RoutePlanningService::MapUnavailable();
    RoutePlanningService::MapClosed();
    const auto closed=RoutePlanningService::View();
    Check(!closed.handDrawnActive&&closed.handDrawnCount==0,
        "confirmed map closure cancels an active drawing even after suspension cleared observedScene");
    RoutePlanningService::MapClosed();
    Check(RoutePlanningService::View().revision==closed.revision,"repeated map-closed observations are idempotent");

    RoutePlanningService::ObserveMap(1,original.stops);
    Command({{"action","handStart"},{"sceneId",1},{"category","collectible"}});
    Command({{"action","handPoint"},{"x",1234.0},{"y",5678.0}});
    Command({{"action","handFinish"}});
    RoutePlanningService::MapUnavailable();
    RoutePlanningService::MapClosed();
    Check(RoutePlanningService::View().handDrawnPending&&RoutePlanningService::View().handDrawnCount==1,
        "confirmed map closure retains a draft explicitly finished before closing");
    RoutePlanningService::ObserveMap(1,original.stops);
    Command({{"action","handDiscard"}});

    Command({{"action","new"},{"sceneId",1}});
    Command({{"action","setStart"},{"sceneId",1},{"x",100.0},{"y",0.0}});
    Command({{"action","add"},{"keys",Json::array({AutoRoute::Key(original.stops.back())})}});
    Command({{"action","generate"}});
    const auto deadline=Clock::now()+3s;
    while(!RoutePlanningService::View().preview&&Clock::now()<deadline)std::this_thread::sleep_for(10ms);
    const auto planning=RoutePlanningService::View();
    Check(planning.preview.has_value(),"selection suspension fixture has a generated preview");
    RoutePlanningService::MapUnavailable();
    const auto pausedPlanning=RoutePlanningService::View();
    Check(pausedPlanning.enabled&&SelectionKeys(pausedPlanning.selected)==SelectionKeys(planning.selected)&&
        pausedPlanning.preview&&planning.preview&&AutoRoute::SameOrder(pausedPlanning.preview->stops,planning.preview->stops),
        "temporary unavailability preserves selection mode, selected points and generated preview");
    RoutePlanningService::ObserveMap(1,original.stops);
    Command({{"action","end"}});

    Command({{"action","handStart"},{"sceneId",1},{"category","collectible"}});
    Command({{"action","handPoint"},{"x",1234.0},{"y",5678.0}});
    RoutePlanningService::MapUnavailable();
    RoutePlanningService::ObserveMap(2,{});
    Check(!RoutePlanningService::Command({{"action","handPoint"},{"x",10.0},{"y",20.0}}).value("accepted",false)&&
        RoutePlanningService::View().handDrawnCount==1,"a different map cannot append points to a retained draft");
    RoutePlanningService::ObserveMap(1,original.stops);
    Check(RoutePlanningService::View().handDrawnActive&&RoutePlanningService::View().handDrawnCount==1,
        "returning from another map preserves the original draft");
    RoutePlanningService::MapUnavailable();
    RoutePlanningService::SessionStopped();
    Check(!RoutePlanningService::View().handDrawnActive&&RoutePlanningService::View().handDrawnCount==0,
        "stopping the game session still retires its unsaved active draft");

    RoutePlanningService::ObserveMap(1,original.stops);
    Command({{"action","handStart"},{"sceneId",1},{"category","collectible"}});
    Command({{"action","handPoint"},{"x",1234.0},{"y",5678.0}});
    const auto oldProfileGeneration=RoutePlanningService::View().generation;
    RoutePlanningService::MapUnavailable();
    DrawItemBase::markerProfile="focus-other-profile";
    Command({{"action","state"}});
    Check(!RoutePlanningService::View().handDrawnActive&&RoutePlanningService::View().handDrawnCount==0,
        "switching profile while suspended clears the previous profile's unsaved drawing");
    Check(!RoutePlanningService::Command({{"action","handPoint"},{"profileId","local"},{"x",1.0},{"y",2.0},
        {"expectedGeneration",oldProfileGeneration}}).value("accepted",false),
        "input from the old profile is rejected after a suspended profile switch");
    DrawItemBase::markerProfile="local";
    Command({{"action","state"}});
}

void VerifyViewportSelection(const AutoRoute::Plan& source){
    // Run after the live replan assertions, using their existing catalog but a new
    // draft. No new worker job or completion mutation can affect those assertions.
    RoutePlanningService::SetAutoReplanEnabled(false);
    const auto activeBefore=RoutePlanningService::View().active;
    const auto& unobscured=source.stops.at(0);
    const auto& completed=source.stops.at(1);
    const auto& behindTools=source.stops.at(2);
    const auto& outside=source.stops.at(3);
    auto layered=unobscured;
    layered.itemId="layered";
    layered.layer.floorId="16";
    layered.layer.level="-1/16";
    const auto layeredKey=AutoRoute::Key(layered);
    const double width=800,height=500;
    const Coordinate exposedPosition{120,120},coveredPosition{500,400};
    const auto build=[&](bool toolsVisible){
        AutoRoute::ViewportCandidates result;
        result.Add(outside,{-1,100},width,height,false,false);
        result.Add(outside,{width+1,100},width,height,false,false);
        result.Add(outside,{100,-1},width,height,false,false);
        result.Add(outside,{100,height+1},width,height,false,false);
        result.Add(outside,{std::numeric_limits<double>::quiet_NaN(),100},width,height,false,false);
        result.Add(outside,{100,std::numeric_limits<double>::infinity()},width,height,false,false);
        result.Add(outside,{-std::numeric_limits<double>::infinity(),100},width,height,false,false);
        result.Add(completed,{250,250},width,height,true,toolsVisible);
        result.Add(unobscured,exposedPosition,width,height,false,false);
        result.Add(behindTools,coveredPosition,width,height,false,toolsVisible);
        result.Add(layered,{300,200},width,height,false,false);
        result.Add(unobscured,exposedPosition,width,height,false,false);
        return result;
    };
    const auto covered=build(true),uncovered=build(false);
    const std::vector<ItemDatas> expected{unobscured,behindTools};
    const auto expectedKeys=SelectionKeys(expected);
    auto expectedVisible=expected;expectedVisible.push_back(layered);
    const auto expectedVisibleKeys=SelectionKeys(expectedVisible);
    Check(SelectionKeys(covered.inViewport)==expectedVisibleKeys,
        "viewport visibility retains the obscured and layered markers but excludes offscreen, nonfinite, completed and duplicate input");
    Check(SelectionKeys(uncovered.inViewport)==expectedVisibleKeys&&
        SelectionKeys(covered.inViewport)==SelectionKeys(uncovered.inViewport),
        "opening or closing tools cannot change viewport visibility identities");
    Check(covered.onCanvas.size()==1&&AutoRoute::Key(covered.onCanvas.front())==AutoRoute::Key(unobscured)&&
        covered.canvasPositions.size()==1&&covered.canvasPositions.front().x==exposedPosition.x&&covered.canvasPositions.front().y==exposedPosition.y,
        "box and lasso gesture candidates exclude layered and tools-covered markers with aligned positions");
    Check(SelectionKeys(uncovered.onCanvas)==expectedKeys&&uncovered.canvasPositions.size()==2&&
        uncovered.canvasPositions[0].x==exposedPosition.x&&uncovered.canvasPositions[0].y==exposedPosition.y&&
        uncovered.canvasPositions[1].x==coveredPosition.x&&uncovered.canvasPositions[1].y==coveredPosition.y,
        "closing the tools restores the ground gesture candidates with their original aligned positions");
    AutoRoute::ViewportCandidates coincident;
    coincident.Add(unobscured,exposedPosition,width,height,false,false);
    coincident.Add(behindTools,exposedPosition,width,height,false,false);
    Check(SelectionKeys(coincident.inViewport)==expectedKeys&&SelectionKeys(coincident.onCanvas)==expectedKeys&&
        coincident.canvasPositions.size()==2&&coincident.canvasPositions[0].x==coincident.canvasPositions[1].x&&
        coincident.canvasPositions[0].y==coincident.canvasPositions[1].y,
        "same-coordinate markers retain both distinct identities in bulk and gesture candidate sets");

    Command({{"action","new"},{"sceneId",source.sceneId}});
    const auto pointTool = RoutePlanningService::Command({{"action","tool"},{"tool","point"}});
    Check(pointTool.value("accepted",false)&&RoutePlanningService::View().tool=="point",
        "the route service accepts the internal single-point selection tool");
    const auto togglePoint=[&](const ItemDatas& point){
        const auto view=RoutePlanningService::View();
        return RoutePlanningService::TogglePoint(point,{{"profileId",view.profileId},
            {"expectedSceneId",view.sceneId},{"expectedGeneration",view.generation}});
    };
    auto toggled=togglePoint(unobscured);
    Check(toggled.value("accepted",false)&&RoutePlanningService::View().selected.size()==1&&
        AutoRoute::Key(RoutePlanningService::View().selected.front())==AutoRoute::Key(unobscured),
        "point toggle adds a previously unselected route target");
    toggled=togglePoint(unobscured);
    Check(toggled.value("accepted",false)&&RoutePlanningService::View().selected.empty(),
        "toggling an already selected route target removes it");
    toggled=togglePoint(unobscured);
    Check(toggled.value("accepted",false)&&RoutePlanningService::View().selected.size()==1,
        "a removed route target can be selected again exactly once");
    Command({{"action","undo"}});
    Check(RoutePlanningService::View().selected.empty(),
        "undo reverses a single-point selection toggle");
    toggled=togglePoint(layered);
    Check(toggled.value("accepted",false)&&RoutePlanningService::View().selected.size()==1&&
        RoutePlanningService::View().selected.front().layer.floorId=="16",
        "explicit single-point selection continues to allow layered targets");
    Command({{"action","clear"}});
    RoutePlanningService::ObserveMap(source.sceneId,covered.inViewport);
    const auto addVisible=[](){
        const auto view=RoutePlanningService::View();
        return Command({{"action","addVisible"},{"profileId",view.profileId},
            {"expectedSceneId",view.sceneId},{"expectedGeneration",view.generation},{"expectedRevision",view.revision}});
    };
    addVisible();
    auto selected=RoutePlanningService::View();
    auto expectedAfterBulk=expected;
    const auto expectedAfterBulkKeys=SelectionKeys(expectedAfterBulk);
    Check(SelectionKeys(selected.selected)==expectedAfterBulkKeys&&selected.hiddenCount==0,
        "addVisible adds only ground points and excludes layered markers");
    addVisible();
    Check(SelectionKeys(RoutePlanningService::View().selected)==expectedAfterBulkKeys,
        "repeated addVisible does not duplicate ground points and continues to exclude layered markers");
    RoutePlanningService::ObserveMap(source.sceneId,uncovered.inViewport);
    Check(RoutePlanningService::View().hiddenCount==0&&SelectionKeys(RoutePlanningService::View().selected)==expectedAfterBulkKeys,
        "collapsing the tools does not make previously selected viewport points become hidden or alter the draft");
    Command({{"action","undo"}});
    Check(RoutePlanningService::View().selected.empty(),
        "one undo reverses the entire viewport append after repeated addition and tools collapse");

    // Preserve a previously selected offscreen member, so hidden-count stability
    // and undo are checked against a nonempty prior draft as well. A deliberately
    // selected layered point must remain visible, and normal one-point additions
    // must continue to be able to include it.
    Command({{"action","add"},{"keys",std::vector<std::string>{layeredKey}}});
    RoutePlanningService::ObserveMap(source.sceneId,covered.inViewport);
    Check(RoutePlanningService::View().hiddenCount==0,
        "a previously selected layered marker remains visible in the full viewport visibility set");
    Command({{"action","add"},{"keys",std::vector<std::string>{AutoRoute::Key(outside)}}});
    const auto before=RoutePlanningService::View().selected;
    RoutePlanningService::ObserveMap(source.sceneId,covered.inViewport);
    Check(RoutePlanningService::View().hiddenCount==1,"an existing genuinely offscreen selection is counted as hidden");
    addVisible();
    auto combined=expectedAfterBulk;combined.push_back(layered);combined.push_back(outside);
    Check(SelectionKeys(RoutePlanningService::View().selected)==SelectionKeys(combined)&&RoutePlanningService::View().hiddenCount==1,
        "bulk append retains the previous offscreen selection and does not count tools coverage as hidden");
    RoutePlanningService::ObserveMap(source.sceneId,uncovered.inViewport);
    Check(RoutePlanningService::View().hiddenCount==1,
        "removing tools coverage leaves the true offscreen hidden count unchanged");
    Command({{"action","undo"}});
    selected=RoutePlanningService::View();
    const auto restoredOutside=std::find_if(selected.selected.begin(),selected.selected.end(),
        [&](const ItemDatas& point){return AutoRoute::Key(point)==AutoRoute::Key(outside);});
    Check(SelectionKeys(selected.selected)==SelectionKeys(before)&&selected.selected.size()==2&&
        restoredOutside!=selected.selected.end()&&restoredOutside->itemMapROC.x==outside.itemMapROC.x&&
        restoredOutside->itemMapROC.y==outside.itemMapROC.y&&selected.hiddenCount==1,
        "undo restores the exact nonempty prior selection and its original coordinates and hidden count");
    Check(activeBefore&&selected.active&&selected.active->id==activeBefore->id&&
        AutoRoute::SameOrder(selected.active->stops,activeBefore->stops)&&selected.active->skipHistory==activeBefore->skipHistory,
        "viewport-only draft regression leaves the previously tested active route and undo-skip history unchanged");
}
// 「攻略内长按跳过当前导航目标」的原生守卫：攻略窗口只能跳过核心当前指向的那个点。
// key / routeId / expectedRevision 都是可选字段，但一旦带上就必须与核心当前状态一致；
// 身份与修订号紧挨着读取和使用，因为服务在后台仍会推进修订号，测试不能依赖两个独立读数之间没有变化。
void VerifyGuideSkipGuard(const AutoRoute::Plan& original){
    const auto initial=RoutePlanningService::View();
    Check(initial.active&&initial.currentTargetIndex>=0,"guide skip guard needs a current navigation target");
    const auto targetView=RoutePlanningService::GuideTarget({{"profileId","local"},{"screenX",40},{"screenY",100}});
    const auto data=targetView.at("data");
    Check(targetView.value("accepted",false)&&data.contains("navigationStatus")&&
        data.at("navigationStatus").get<std::string>()==initial.navigationStatus,
        "the guide target reply publishes the navigation status the managed side authorises a skip with");
    Check(!data.value("routeId",std::string{}).empty()&&data.contains("revision")&&data.at("selection").is_object(),
        "the guide target reply still carries the route identity, revision and the current target");
    const auto routeId=data.at("routeId").get<std::string>();

    // 不是当前目标的点位：带上的 key 与核心目标不一致，必须整条拒绝。
    {
        const auto before=RoutePlanningService::View();
        const auto staleKey=AutoRoute::Key(before.active->stops[before.currentTargetIndex])+":not-the-target";
        const auto result=RoutePlanningService::Command({{"action","skip"},{"profileId","local"},{"routeId",routeId},
            {"key",staleKey},{"expectedRevision",before.revision}});
        const auto after=RoutePlanningService::View();
        Check(!result.value("accepted",false)&&after.active->skipped==before.active->skipped&&
            after.currentTargetIndex==before.currentTargetIndex,
            "a skip for a point that is not the current target is rejected without changing progress");
    }
    // 其它路线：routeId 与活动路线不符。
    {
        const auto before=RoutePlanningService::View();
        const auto result=RoutePlanningService::Command({{"action","skip"},{"profileId","local"},{"routeId","other-route"},
            {"key",AutoRoute::Key(before.active->stops[before.currentTargetIndex])},{"expectedRevision",before.revision}});
        const auto after=RoutePlanningService::View();
        Check(!result.value("accepted",false)&&after.active->skipped==before.active->skipped,
            "a skip bound to another route is rejected without changing progress");
    }
    // 过期的修订号：窗口拿着旧答复提交时不能生效。
    {
        const auto before=RoutePlanningService::View();
        const auto result=RoutePlanningService::Command({{"action","skip"},{"profileId","local"},{"routeId",routeId},
            {"key",AutoRoute::Key(before.active->stops[before.currentTargetIndex])},{"expectedRevision",before.revision+7}});
        const auto after=RoutePlanningService::View();
        Check(!result.value("accepted",false)&&after.active->skipped==before.active->skipped,
            "a skip carrying a stale revision is rejected without changing progress");
    }
    // 当前目标本身：接受，只写路线进度，并把当前目标推进到下一个。
    {
        const auto before=RoutePlanningService::View();
        const auto key=AutoRoute::Key(before.active->stops[before.currentTargetIndex]);
        const auto result=RoutePlanningService::Command({{"action","skip"},{"profileId","local"},{"routeId",routeId},
            {"key",key},{"expectedRevision",before.revision}});
        const auto after=RoutePlanningService::View();
        Check(result.value("accepted",false)&&after.active->skipped.count(key)==1&&after.currentTargetIndex>=0&&
            AutoRoute::Key(after.active->stops[after.currentTargetIndex])!=key,
            ("the current navigation target can be skipped and the route advances "+
                result.value("message",std::string{"<none>"})).c_str());
        const auto undo=RoutePlanningService::Command({{"action","undoSkip"}});
        const auto undone=RoutePlanningService::View();
        // 只和"跳过之前"比较：本用例不改动更早的历史，撤一次必须精确回到那一刻。
        Check(undo.value("accepted",false)&&undone.active->skipped==before.active->skipped&&
            undone.active->skipHistory==before.active->skipHistory&&
            AutoRoute::Key(undone.active->stops[undone.currentTargetIndex])==key,
            "the skipped guide target can be undone and restores the route progress from just before the skip");
        // 历史已经用尽：再撤一次必须被拒，而不是去改别的点位状态。
        const auto secondUndo=RoutePlanningService::Command({{"action","undoSkip"}});
        Check(!secondUndo.value("accepted",false)&&RoutePlanningService::View().active->skipped==before.active->skipped,
            "an exhausted skip history rejects the extra undo and leaves route progress alone");
    }
}
// 路线工具栏的方向选择：屏幕上 y 向下增长，同一行里的按钮必须能左右移动。
// 这条用例存在的理由：实机报告"左摇杆只能上下切换"，需要能直接证伪或证实。
void VerifyRouteToolbarNavigation(){
    const auto button=[](const char* key,double left,double top,double right,double bottom){
        MarkerHitRegion region;region.key=key;region.left=left;region.top=top;region.right=right;region.bottom=bottom;return region;};
    // 三行按钮，每行三个，行内水平间距 40，行间距 50。
    const std::vector<MarkerHitRegion> buttons{
        button("route:tool:pan",0,0,100,40),button("route:tool:point",140,0,240,40),button("route:tool:box",280,0,380,40),
        button("route:tool:lasso",0,90,100,130),button("route:tool:start",140,90,240,130),button("route:addVisible",280,90,380,130),
        button("route:undo",0,180,100,220),button("route:clear",140,180,240,220),button("route:generate",280,180,380,220)};
    Check(RouteToolbarNextKey(buttons,"route:tool:point",-1)=="route:tool:pan",
        "a left press selects the button on the left in the same row");
    Check(RouteToolbarNextKey(buttons,"route:tool:point",1)=="route:tool:box",
        "a right press selects the button on the right in the same row");
    Check(RouteToolbarNextKey(buttons,"route:tool:pan",-1)=="route:tool:pan",
        "the leftmost button of a row has no left neighbour");
    Check(RouteToolbarNextKey(buttons,"route:tool:box",1)=="route:tool:box",
        "the rightmost button of a row has no right neighbour");
    Check(RouteToolbarNextKey(buttons,"route:tool:point",-2)=="route:tool:point",
        "an up press needs a button in a row above, not just anywhere");
    Check(RouteToolbarNextKey(buttons,"route:tool:lasso",-2)=="route:tool:pan",
        "an up press moves to the row above");
    Check(RouteToolbarNextKey(buttons,"route:tool:pan",2)=="route:tool:lasso",
        "a down press moves to the row below");
    Check(RouteToolbarNextKey(buttons,"route:missing",1)==buttons.front().key,
        "an unknown selected key falls back to the first button");
    Check(RouteToolbarNextKey({},"route:tool:pan",1)=="route:tool:pan",
        "an empty toolbar keeps the current selection");
}
// 排版 + 选择的组合验证：用真正的 OverlayPanel::Pack 排按钮，再按真实几何做方向选择。
// 只有这一层被验过，"算法正确但实机没有水平相邻按钮"这种可能才算排除掉。
void VerifyToolbarLayoutNavigation(){
    const auto layoutFor=[](const std::vector<OverlayPanel::ButtonMeasure>& measures){
        const auto layout=OverlayPanel::Pack(1920.0f,0.0f,1.0f,measures,true,1);
        std::vector<MarkerHitRegion> regions;
        for(std::size_t i=0;i<layout.buttons.size();++i){
            const auto& box=layout.buttons[i];
            MarkerHitRegion region;region.left=box.left;region.top=box.top;region.right=box.right;region.bottom=box.bottom;
            region.key="route:"+std::to_string(i);regions.push_back(region);
        }
        return regions;};
    // 九个等宽按钮（含分组换行），1920 宽下应当排成多行。
    std::vector<OverlayPanel::ButtonMeasure> measures;
    for(int i=0;i<9;++i) measures.push_back({140.0f,i<5?0:1});
    const auto regions=layoutFor(measures);
    Check(regions.size()==9,"the test layout keeps every measured button");
    const auto sameRow=[](const MarkerHitRegion& a,const MarkerHitRegion& b){
        return std::abs((a.top+a.bottom)/2-(b.top+b.bottom)/2)<1.0;};
    Check(sameRow(regions[0],regions[1])&&regions[1].left>regions[0].right,
        "the first two buttons share a row and sit side by side");
    Check(RouteToolbarNextKey(regions,regions[0].key,1)==regions[1].key,
        "a real packed layout moves right to the next button in the row");
    Check(RouteToolbarNextKey(regions,regions[1].key,-1)==regions[0].key,
        "a real packed layout moves left to the previous button in the row");
    Check(!sameRow(regions[0],regions[8]),"the last button lands in a different row");
    Check(RouteToolbarNextKey(regions,regions[0].key,2)==regions[5].key,
        "a real packed layout moves down into the next row");
}
// The farming mode end to end through the real service: the toolbar command arms it, the
// proximity observation drives the dwell, and the write lands in the ledger. What has to
// hold is as much about what it does NOT mark — a mode that is off, a target out of range,
// and a target that is not a daily-refresh point whose completion can never be given back.
void VerifyFarmMode(){
    RoutePlanningService::Shutdown();
    const auto* scene=Scene::Find(1);
    const auto state=std::to_string(scene->kuroStateId);
    Json points=Json::array();
    for(const auto& pair:std::vector<std::pair<std::string,double>>{{"near-a",100},{"near-b",110},{"chest",120},{"far",500}})
        points.push_back({{"id",pair.first},{"x",pair.second/scene->scale*100},{"y",0},{"stateId",scene->kuroStateId}});
    DrawItemBase::itemsJsonData_World=Json::array({{{"id","mob"},{"name","test"},{"location",points}}});
    DrawItemBase::completed.clear();
    DrawItemBase::refreshablePointIds.clear();
    DrawItemBase::refreshablePointIds.insert("near-a");
    DrawItemBase::refreshablePointIds.insert("near-b");
    RoutePlanningService::Initialize();
    Command({{"action","new"},{"sceneId",1}});
    Command({{"action","setStart"},{"sceneId",1},{"x",100},{"y",0}});
    std::vector<std::string> keys;for(const auto* id:{"near-a","near-b","chest","far"})keys.push_back(state+":"+id);
    Command({{"action","add"},{"keys",keys}});Command({{"action","generate"}});
    const auto deadline=Clock::now()+3s;
    while(!RoutePlanningService::View().preview&&Clock::now()<deadline)std::this_thread::sleep_for(10ms);
    Command({{"action","activate"}});Command({{"action","resume"}});
    const auto plan=*RoutePlanningService::View().active;
    const auto keyOf=[&](const std::string& id){for(const auto& stop:plan.stops)if(stop.itemId==id)return AutoRoute::Key(stop);return std::string{};};
    Check(keyOf("near-a").empty()==false&&keyOf("near-b").empty()==false&&keyOf("chest").empty()==false,
        "the farming fixture must hold every target it plans to check");

    // One observation of the whole cluster from one player position, exactly as the overlay
    // builds it: the current target plus every later target of the same route.
    const auto observe=[&](double x){
        const auto now=Clock::now();const auto seq=++sequence;
        RoutePlanningService::UpdatePlayer({1,{x,0},"playerSnapshot",0,1,true});
        RoutePlanningService::SetPlayerAvailable(true);
        RoutePlanningService::ObservePlayer({"local",1,1,seq,1,{x,0},now,now,true,true});
        const auto view=RoutePlanningService::View();
        if(!view.active||view.currentTargetIndex<0)return;
        AutoRoute::ProximityObservation value;
        value.profileId="local";value.routeId=view.active->id;
        value.targetKey=AutoRoute::Key(view.active->stops[view.currentTargetIndex]);
        value.sessionId=1;value.orderRevision=view.orderRevision;value.sourceFrameId=seq;
        value.sceneId=1;value.capturedAt=now;value.presentedAt=now;value.valid=true;
        value.distancePixels=AutoRoute::TargetDistancePixels(view.active->stops[view.currentTargetIndex].itemMapROC,
            {x,0},{100,100},1,{});
        for(std::size_t index=static_cast<std::size_t>(view.currentTargetIndex);index<view.active->stops.size();++index){
            const auto& stop=view.active->stops[index];
            if(view.completed.contains(AutoRoute::Key(stop))||view.active->skipped.contains(AutoRoute::Key(stop)))continue;
            value.nearby.push_back({AutoRoute::Key(stop),AutoRoute::TargetDistancePixels(stop.itemMapROC,{x,0},{100,100},1,{})});
        }
        RoutePlanningService::ObserveProximity(value);
    };
    const auto dwell=[&](double x,std::chrono::milliseconds total){
        const auto until=Clock::now()+total;
        do{observe(x);std::this_thread::sleep_for(60ms);}while(Clock::now()<until);
    };

    // Mode off: a full dwell sitting on a refreshable target still marks nothing. This is the
    // promise the toolbar keeps printing whenever the mode is off.
    const auto baseline=RoutePlanningService::View().completed;
    dwell(100,700ms);
    Check(RoutePlanningService::View().completed==baseline,
        "a full dwell with the farming mode off must mark nothing at all");

    Command({{"action","farm"},{"enabled",true}});
    Check(RoutePlanningService::View().farmMode,"the toolbar command arms the farming mode");
    Check(RoutePlanningService::View().completed==baseline,"arming the mode must not mark anything by itself");
    const auto serialBefore=RoutePlanningService::View().farmNoticeSerial;

    // One frame in range is not a decision: the player may be running past the point.
    observe(100);
    Check(!RoutePlanningService::View().completed.contains(keyOf("near-a")),
        "one frame in range must not mark a target");

    // A sustained dwell marks the refreshable targets of the cluster, and only those.
    dwell(100,700ms);
    const auto after=RoutePlanningService::View();
    Check(after.completed.contains(keyOf("near-a"))&&after.completed.contains(keyOf("near-b")),
        "a dwell in range must mark every daily-refresh target of the cluster in one batch");
    Check(!after.completed.contains(keyOf("chest")),
        "a target that is not a daily-refresh point must never be auto-marked");
    Check(!after.completed.contains(keyOf("far")),"a target out of range must not be marked");
    Check(after.farmNotice=="刷怪采集模式自动标记 2 个","the toolbar must report the batch it just marked");
    Check(after.farmNoticeSerial!=serialBefore,"a new batch must be distinguishable from the last one shown");

    // The remaining target is the one-off collectible, so nothing further happens even though
    // the player is standing inside its range.
    dwell(120,700ms);
    Check(!RoutePlanningService::View().completed.contains(keyOf("chest")),
        "a route that ends on a collectible must not be advanced by the farming mode");

    // "退出导航" is navigation stopping, not "any toolbar action that leaves the navigation
    // buttons". Opening the point editor keeps the route active, so the mode has to survive it.
    Command({{"action","new"},{"sceneId",1}});
    Check(RoutePlanningService::View().active.has_value()&&RoutePlanningService::View().farmMode,
        "entering the point editor must not end the navigation or the farming mode");
    Command({{"action","end"}});
    Check(RoutePlanningService::View().farmMode,"leaving the point editor must not end the farming mode either");

    // Leaving the navigation turns the mode off, so a later route cannot inherit it.
    Command({{"action","stop"}});
    Check(!RoutePlanningService::View().farmMode,"leaving the navigation must turn the farming mode off");

    // But the setting itself lives on the route: the player built this route to farm, so
    // starting it again arms the mode again without touching the toolbar button.
    Command({{"action","load"},{"routeId",plan.id}});
    Check(RoutePlanningService::View().farmMode,"loading a farming route must arm the mode again");
    Command({{"action","resume"}});Command({{"action","stop"}});
    Check(!RoutePlanningService::View().farmMode,"resuming and stopping again still ends the runtime mode");

    // Switching it off is remembered just as well, including on disk.
    Command({{"action","load"},{"routeId",plan.id}});
    Check(RoutePlanningService::View().farmMode,"fixture must load the route armed");
    Command({{"action","farm"},{"enabled",false}});
    Check(!RoutePlanningService::View().farmMode,"the toolbar toggle must also write the setting off");
    Command({{"action","stop"}});
    Command({{"action","load"},{"routeId",plan.id}});
    Check(!RoutePlanningService::View().farmMode,"a route switched back off must stay off when it is loaded again");
    const auto routePath=StructuredLogger::root/"SavedRoutes"/"Auto"/"local"/(plan.id+".json");
    {std::ifstream input(routePath);const auto document=Json::parse(input);
        Check(document.contains("farmMode")&&!document.at("farmMode").get<bool>(),
            "the route file is what remembers the setting, and it now records it as off");}

    // A replan of the active route is still the same route: 重新规划 followed by 开始指引 must
    // not quietly switch the farming mode off while the player is standing on a farming route.
    Command({{"action","farm"},{"enabled",true}});
    // 重新规划 is only reachable from the big map, so the test enters it the way the overlay
    // does: a captured map start plus an observed scene. Without that the replan has no start
    // to solve from, which is what the real service reports as "无法取得当前位置".
    RoutePlanningService::CaptureMapStart({1,{100,0},"manual",0,1,true});
    RoutePlanningService::ObserveMap(1,{});
    Command({{"action","replan"}});
    {
        const auto until=Clock::now()+3s;
        while(!RoutePlanningService::View().preview&&Clock::now()<until)std::this_thread::sleep_for(10ms);
        Check(RoutePlanningService::View().preview.has_value(),"fixture must produce a replanned preview");
    }
    Command({{"action","activate"}});
    Check(RoutePlanningService::View().farmMode,
        "starting the route a replan produced must keep the farming setting of the route it replaced");
    Command({{"action","stop"}});

    // A second route that never asked to farm must turn the mode back off rather than inherit
    // the setting of whichever route was loaded before it.
    Command({{"action","load"},{"routeId",plan.id}});
    Command({{"action","farm"},{"enabled",true}});
    Check(RoutePlanningService::View().farmMode,"fixture must leave the first route armed");
    {
        AutoRoute::RoutePlanStore otherStore(StructuredLogger::root/"SavedRoutes"/"Auto");
        auto plain=*RoutePlanningService::View().active;
        plain.id="plain-route";plain.name="plain";plain.farmMode=false;
        otherStore.Save(plain,false);
        Command({{"action","load"},{"routeId","plain-route"}});
        Check(!RoutePlanningService::View().farmMode,
            "a route saved without the farming setting must not inherit the previous route's mode");
    }
}
// A route package is what one player hands to another. It holds the very documents the store writes,
// because a route that survives being saved has to survive being carried; and the file itself says
// whether it is a whole collection or a handful of routes, so importing never asks the player to
// classify a file the file already describes.
void VerifyTypedFreeRoutes(const AutoRoute::Plan& original) {
    Command({{"action","collectionNew"},{"name","自由点回归测试"}});
    Command({{"action","handCancel"}});
    Command({{"action","handStart"}});
    Check(RoutePlanningService::View().handDrawnTypeChoosing,"a blank shortcut opens the type chooser");
    Command({{"action","handStart"},{"category","daily"}});
    Check(!RoutePlanningService::View().enabled,"hand drawing exits the selection tool so clicks belong to the hand draft");
    const auto rejected=RoutePlanningService::Command({{"action","handPoint"},{"x",100.0},{"y",0.0},{"key",std::to_string(original.stops.front().layer.stateId)+":alpha"}});
    Check(!rejected.value("accepted",false)&&rejected.value("message",std::string{}).find("不匹配")!=std::string::npos&&RoutePlanningService::View().handDrawnCount==0,"a collectible official point is refused by a daily route without dropping a replacement free point");
    Command({{"action","handIcon"},{"icon","monster3C"}});
    Command({{"action","handPoint"},{"x",10000.0},{"y",10000.0}});
    const auto changedType=RoutePlanningService::Command({{"action","handStart"},{"category","collectible"}});
    Check(!changedType.value("accepted",false),"route type locks after the first point");
    Command({{"action","handIcon"},{"icon","plant"}});
    Command({{"action","handPoint"},{"x",10100.0},{"y",10000.0}});
    auto draft=RoutePlanningService::View();const auto id=draft.handDraftPreview->stops.front().freeRouteId;
    Check(draft.handDraftPreview->stops[0].freeIcon==FreePointIcon::Monster3C&&draft.handDraftPreview->stops[1].freeIcon==FreePointIcon::Plant,"icon selection preserves earlier free points");
    Command({{"action","handFinish"}});Command({{"action","handStart"}});
    Check(RoutePlanningService::View().handIcon==FreePointIcon::Plant,"resuming preserves the selected icon");
    Command({{"action","handFinish"}});Command({{"action","handCommit"},{"name","Typed free test"}});
    Command({{"action","switch"},{"routeId",id}});
    auto view=RoutePlanningService::View();const auto point=view.active->stops[0];
    auto done=RoutePlanningService::CompleteFreePoint({{"profileId","local"},{"routeId",id},{"pointId",point.itemId},{"stateId",point.layer.stateId},{"completed",true},{"automatic",true}});
    Check(!done.value("accepted",false),"automatic free completion stays off until farming is enabled");
    Command({{"action","complete"},{"routeId",id},{"key",AutoRoute::Key(point)},{"profileId","local"}});
    Check(RoutePlanningService::View().currentTargetIndex==1,"manual free completion advances to the next target");
    done=RoutePlanningService::CompleteFreePoint({{"profileId","local"},{"routeId",id},{"pointId",point.itemId},{"stateId",point.layer.stateId},{"completed",false}});
    Check(done.value("accepted",false)&&RoutePlanningService::View().currentTargetIndex==0,"undo free completion restores the target");
    done=RoutePlanningService::CompleteFreePoint({{"profileId","local"},{"routeId","old-route"},{"pointId",point.itemId},{"stateId",point.layer.stateId},{"completed",true}});
    Check(!done.value("accepted",false),"a stale route request cannot complete the new route");
    Command({{"action","farm"},{"enabled",true}});
    done=RoutePlanningService::CompleteFreePoint({{"profileId","local"},{"routeId",id},{"pointId",point.itemId},{"stateId",point.layer.stateId},{"completed",true},{"automatic",true}});
    Check(done.value("accepted",false),"daily free targets participate in opt-in farming");
    Command({{"action","handStart"},{"category","collectible"}});
    Check(!RoutePlanningService::Command({{"action","handIcon"},{"icon","ore"}}).value("accepted",false),"collectible drawing only accepts number icons");
    Command({{"action","handCancel"}});
    RoutePlanningService::CompleteFreePoint({{"profileId","local"},{"routeId",id},{"pointId",point.itemId},{"stateId",point.layer.stateId},{"completed",false}});
    Command({{"action","replan"}});
    const auto deadline=Clock::now()+3s;
    while(!RoutePlanningService::View().preview&&Clock::now()<deadline)std::this_thread::sleep_for(10ms);
    Check(RoutePlanningService::View().preview.has_value(),"free-point routes can be manually replanned");
    Command({{"action","activate"}});
    const auto replanned=*RoutePlanningService::View().active;
    Check(replanned.id!=id&&std::all_of(replanned.stops.begin(),replanned.stops.end(),[&](const auto& stop){return stop.freeRouteId==replanned.id;}),"replanning rebinds every live free stop to the new route owner");
    const auto newPoint=replanned.stops.front();
    auto completed=RoutePlanningService::CompleteFreePoint({{"profileId","local"},{"routeId",replanned.id},{"pointId",newPoint.itemId},{"stateId",newPoint.layer.stateId},{"completed",true}});
    Check(completed.value("accepted",false)&&!DrawItemBase::IsPointCompleted("World",point),"completing a replanned free point never completes the original route");
    const auto exportPath=StructuredLogger::root/"free-overwrite.json";
    Command({{"action","export"},{"path",AutoRoute::Utf8Text(exportPath)},{"collectionId",replanned.collection}});
    Command({{"action","importInspect"},{"path",AutoRoute::Utf8Text(exportPath)}});
    Command({{"action","importApply"},{"path",AutoRoute::Utf8Text(exportPath)},{"mode","collectionOverwrite"}});
    Check(!RoutePlanningService::View().active,"overwriting the active collection invalidates its removed route and local target");
    Command({{"action","switch"},{"routeId",replanned.id}});
    const auto imported=RoutePlanningService::View();
    Check(imported.completed.empty()&&imported.active->stops.front().itemId!=newPoint.itemId,"overwritten imported free points receive fresh identities and no previous progress");
}
void VerifyRouteBundle(){
    // Collections live in memory for the life of the process, so a clean slate is made through the
    // service rather than by rewriting the file behind its back.
    const auto before=[&]{return RoutePlanningService::Snapshot().at("collections");}();
    for(const auto& row:before)
        if(!row.value("system",false))
            RoutePlanningService::Command({{"action","collectionDelete"},{"collectionId",row.value("id",std::string{})}});
    AutoRoute::RoutePlanStore store(StructuredLogger::root/"SavedRoutes"/"Auto");
    const auto refused=[&](Json command){return !RoutePlanningService::Command(command).value("accepted",false);};
    const auto message=[&](){return RoutePlanningService::Snapshot().value("message",std::string{});};
    const auto exported=[&](const std::filesystem::path& path){
        std::ifstream input(path,std::ios::binary);
        if(!input)return Json(nullptr);
        return Json::parse(std::string(std::istreambuf_iterator<char>(input),{}));
    };
    const auto root=AutoRoute::Utf8Text(StructuredLogger::root);
    // The folder and the files are named in Chinese on purpose: the path arrives from the interface
    // as UTF-8, and a narrow std::filesystem::path would read it in the process code page. Every path
    // here is therefore built from UTF-8 text, never by appending a literal to a path.
    const auto under=[&](const std::string& name){return AutoRoute::Utf8Path(root+"/路线包/"+name);};
    const auto collectionPath=under("宝箱路线.json");
    const auto routesPath=under("选中的路线.json");
    // A template route written by the service itself, so every clone of it resolves against the
    // catalogue this harness installed.
    Command({{"action","new"},{"sceneId",1}});
    Command({{"action","setStart"},{"sceneId",1},{"x",100},{"y",0}});
    {
        const auto* scene=Scene::Find(1);
        std::vector<std::string> keys;
        for(const auto* id:{"alpha","beta","gamma"})keys.push_back(std::to_string(scene->kuroStateId)+":"+id);
        Command({{"action","add"},{"keys",keys}});
    }
    Command({{"action","generate"}});
    {
        const auto deadline=Clock::now()+3s;
        while(!RoutePlanningService::View().preview&&Clock::now()<deadline)std::this_thread::sleep_for(10ms);
    }
    Command({{"action","activate"}});
    const auto template_=*RoutePlanningService::View().active;
    Command({{"action","stop"}});
    const auto write=[&](const std::string& id,const std::string& name,const std::string& collection){
        auto plan=template_;plan.id=id;plan.name=name;plan.collection=collection;
        plan.skipped.clear();plan.skipHistory.clear();store.Save(plan,false);
    };
    const auto rowIdByName=[&](const std::string& name)->std::string{
        const auto rows=RoutePlanningService::Snapshot().at("savedRoutes");
        for(const auto& row:rows)if(row.value("name",std::string{})==name)return row.value("id",std::string{});
        return {};
    };

    Command({{"action","collectionNew"},{"name","待导出"}});
    const auto sourceId=RoutePlanningService::Snapshot().value("currentCollection",std::string{});
    write("bundle-a","甲路线",sourceId);
    write("bundle-b","乙路线",sourceId);
    write("bundle-c","丙路线",AutoRoute::DefaultCollectionId);
    Command({{"action","list"}});

    // Exporting a whole collection.
    Command({{"action","export"},{"path",AutoRoute::Utf8Text(collectionPath)},{"collectionId",sourceId}});
    Check(std::filesystem::exists(collectionPath),"a bundle is written where the path says, Chinese folder and all");
    {
        const auto document=exported(collectionPath);
        Check(document.is_object()&&document.value("kind",std::string{})=="collection"&&
            document.value("formatVersion",0)==2&&document.value("app",std::string{})=="IMao",
            "a collection bundle says what it is and which format it is in");
        Check(document.at("collection").value("name",std::string{})=="待导出",
            "a collection bundle carries the collection's name, which is the only thing that names it on the other side");
        Check(document.at("routes").size()==2,"a collection bundle holds exactly the routes filed in that collection");
        // Verbatim: the bundle route and the stored route have to be the same document, or a route
        // would survive a save and not a transfer.
        std::ifstream stored(AutoRoute::Utf8Path(root+"/SavedRoutes/Auto/local/bundle-a.json"),std::ios::binary);
        const auto storedDocument=Json::parse(std::string(std::istreambuf_iterator<char>(stored),{}));
        const auto inBundle=[&](){
            for(const auto& entry:document.at("routes"))
                if(entry.value("id",std::string{})=="bundle-a")return entry;
            return Json(nullptr);}();
        Check(inBundle==storedDocument,"the route inside a bundle is byte for byte the document the store writes");
    }
    Check(message().find("2 条路线")!=std::string::npos&&message().find("宝箱路线.json")!=std::string::npos,
        "the report says how many routes were exported and which file they went to");

    // Exporting a hand-picked batch produces the other kind, and carries no collection at all.
    Command({{"action","export"},{"path",AutoRoute::Utf8Text(routesPath)},
        {"routeIds",Json::array({"bundle-a","bundle-c"})}});
    {
        const auto document=exported(routesPath);
        Check(document.value("kind",std::string{})=="routes"&&!document.contains("collection"),
            "a bundle of picked routes is not a collection and does not pretend to be one");
        Check(document.at("routes").size()==2,"a bundle of picked routes holds exactly the ones that were picked");
    }

    // The default collection is exportable like any other: it is only special in that it always exists.
    Command({{"action","export"},{"path",AutoRoute::Utf8Text(under("默认合集.json"))},{"collectionId",AutoRoute::DefaultCollectionId}});
    {
        const auto document=exported(under("默认合集.json"));
        bool carried=false;
        for(const auto& entry:document.at("routes"))if(entry.value("id",std::string{})=="bundle-c")carried=true;
        Check(document.value("kind",std::string{})=="collection"&&carried,
            "the default collection exports like any other collection");
    }

    Check(refused({{"action","export"},{"path",AutoRoute::Utf8Text(under("none.json"))},{"routeIds",Json::array()}}),
        "exporting nothing is refused rather than written as an empty bundle");
    Check(refused({{"action","export"},{"path",AutoRoute::Utf8Text(under("none.json"))},{"collectionId","no-such-collection"}}),
        "exporting a collection that does not exist is refused");
    Check(refused({{"action","export"},{"path",""},{"collectionId",sourceId}}),
        "exporting without a destination is refused");
    Check(refused({{"action","export"},{"collectionId",sourceId}}),
        "exporting without a destination at all is refused");
    Check(refused({{"action","export"},{"path",AutoRoute::Utf8Text(under("bad.json"))},{"routeIds",Json::array({"no-such-route"})}}),
        "exporting routes that cannot be read is refused instead of writing an empty bundle");
    Check(!std::filesystem::exists(under("bad.json")),"a refused export writes no file");

    // One damaged route must not cost the player the rest of the collection: what can be read is
    // exported, and the report says that something was left out. The damaged route is the one in the
    // default collection, because a file that cannot be parsed cannot say which collection it was in
    // — reading it there is the only safe answer, and that is what makes it visible to this export.
    WriteTextAtomically(AutoRoute::Utf8Path(root+"/SavedRoutes/Auto/local/bundle-c.json"),"{not a route");
    Command({{"action","list"}});
    Command({{"action","export"},{"path",AutoRoute::Utf8Text(under("部分.json"))},{"collectionId",AutoRoute::DefaultCollectionId}});
    {
        const auto document=exported(under("部分.json"));
        bool carriedDamaged=false;
        for(const auto& entry:document.at("routes"))if(entry.value("id",std::string{})=="bundle-c")carriedDamaged=true;
        Check(!carriedDamaged,"a collection with a damaged route does not carry that route into the bundle");
    }
    Check(message().find("条读不出来")!=std::string::npos,"the report names that some routes were left out");

    // --- importing -------------------------------------------------------------------------------
    const auto state_=[&](){return RoutePlanningService::Snapshot();};
    const auto collections=[&](){return state_().at("collections");};
    const auto collectionRow=[&](const std::string& id)->Json{
        const auto rows=collections();
        for(const auto& row:rows)if(row.value("id",std::string{})==id)return row;
        return Json(nullptr);
    };
    const auto routeCount=[&](const std::string& id){return collectionRow(id).at("routeCount").get<int>();};
    const auto currentCollection=[&](){return state_().value("currentCollection",std::string{});};
    const auto transfer=[&](){return state_().at("transfer");};
    const auto rowCollection=[&](const std::string& routeId)->std::string{
        const auto rows=state_().at("savedRoutes");
        for(const auto& row:rows)if(row.value("id",std::string{})==routeId)return row.value("collection",std::string{});
        return {};
    };
    const auto rowName=[&](const std::string& routeId)->std::string{
        const auto rows=state_().at("savedRoutes");
        for(const auto& row:rows)if(row.value("id",std::string{})==routeId)return row.value("name",std::string{});
        return {};
    };
    const auto routeIdByName=[&](const std::string& name)->std::string{
        const auto rows=state_().at("savedRoutes");
        for(const auto& row:rows)if(row.value("name",std::string{})==name)return row.value("id",std::string{});
        return {};
    };
    // The same name can legitimately exist in several collections, so "which route is this" is only
    // answered by the name *and* the collection: an id alone would pick whichever sorts first.
    const auto routeIdIn=[&](const std::string& name,const std::string& collectionId)->std::string{
        const auto rows=state_().at("savedRoutes");
        for(const auto& row:rows)
            if(row.value("name",std::string{})==name&&
                AutoRoute::SameRouteId(row.value("collection",std::string{AutoRoute::DefaultCollectionId}),collectionId))
                return row.value("id",std::string{});
        return {};
    };
    const auto savedIds=[&](){
        std::set<std::string> ids;
        for(const auto& row:state_().at("savedRoutes"))ids.insert(row.value("id",std::string{}));
        return ids;
    };
    // A bundle whose routes carry ids that are free here, so what the import does *with* an id can be
    // told apart from what it does *about* a collision. The read is scoped on purpose: an open
    // std::ifstream shares reading and writing but not deleting, so a stream still alive here would
    // block the atomic replace that writes the file back.
    const auto rewriteIds=[&](const std::filesystem::path& path,const std::vector<std::string>& ids){
        Json document;
        {
            std::ifstream input(path,std::ios::binary);
            document=Json::parse(std::string(std::istreambuf_iterator<char>(input),{}));
        }
        for(std::size_t i=0;i<ids.size();++i)document["routes"][i]["id"]=ids[i];
        WriteTextAtomically(path,document.dump(2));
    };
    const auto collectionBundle=under("导入测试.json");
    const auto routesBundle=under("导入路线.json");
    const auto brokenBundle=under("坏包.json");

    Command({{"action","collectionNew"},{"name","来源"}});
    const auto originId=currentCollection();
    write("import-a","共享甲",originId);
    write("import-b","共享乙",originId);
    Command({{"action","list"}});
    Command({{"action","export"},{"path",AutoRoute::Utf8Text(collectionBundle)},{"collectionId",originId}});
    rewriteIds(collectionBundle,{"shared-x","shared-y"});

    // Inspecting is reading, and nothing else. Everything the player is asked to decide is answered
    // here, so the interface never has to open a route package itself.
    const auto idsBefore=savedIds();
    Command({{"action","importInspect"},{"path",AutoRoute::Utf8Text(collectionBundle)}});
    Check(transfer().is_object()&&transfer().value("kind",std::string{})=="collection"&&
        transfer().value("collectionName",std::string{})=="来源"&&transfer().value("routeCount",0)==2&&
        transfer().value("skipped",0)==0,
        "inspecting a collection bundle answers what it is, what it is called and how much is in it");
    Check(transfer().at("conflict").is_object()&&
        transfer().at("conflict").value("collectionId",std::string{})==originId&&
        transfer().at("conflict").value("routeCount",0)==2,
        "inspecting reports the collection whose name is already taken, with what is in it");
    Check(savedIds()==idsBefore&&currentCollection()==originId&&routeCount(originId)==2,
        "inspecting a bundle changes nothing at all");
    Check(message().find("来源")!=std::string::npos&&message().find("2 条路线")!=std::string::npos,
        "the inspection says what the file holds before anything is decided");

    Check(refused({{"action","importApply"},{"path",AutoRoute::Utf8Text(routesBundle)},{"mode","routes"}}),
        "applying a bundle that was never inspected is refused");
    Check(refused({{"action","importApply"},{"path",AutoRoute::Utf8Text(collectionBundle)},{"mode","routes"}}),
        "a collection bundle cannot be applied as a bundle of routes");
    Check(refused({{"action","importApply"},{"path",AutoRoute::Utf8Text(collectionBundle)},{"mode","nonsense"}}),
        "an import mode nobody recognises is refused");
    Check(!transfer().is_null()&&routeCount(originId)==2,
        "a refused import leaves the pending inspection and the collections as they were");

    // Overwrite: the collection is replaced, and because it is emptied first the ids the bundle
    // carries are free again — which is what makes importing the same package twice settle.
    Command({{"action","importApply"},{"path",AutoRoute::Utf8Text(collectionBundle)},{"mode","collectionOverwrite"}});
    Check(rowCollection("shared-x")==originId&&rowCollection("shared-y")==originId,
        "importing a collection keeps the ids the bundle carries when they are free here");
    Check(rowName("shared-x")=="共享甲"&&rowName("shared-y")=="共享乙",
        "imported routes keep their names when nothing in the collection has taken them");
    Check(routeCount(originId)==2,
        "replacing a collection leaves exactly the imported routes in it");
    Check(currentCollection()==originId,"importing a collection enters it, so the next save lands beside it");
    Check(transfer().is_null(),"the pending inspection is cleared once it has been applied");
    Command({{"action","importInspect"},{"path",AutoRoute::Utf8Text(collectionBundle)}});
    Command({{"action","importApply"},{"path",AutoRoute::Utf8Text(collectionBundle)},{"mode","collectionOverwrite"}});
    Check(rowCollection("shared-x")==originId&&rowCollection("shared-y")==originId&&routeCount(originId)==2,
        "importing the same collection bundle again settles on the same routes instead of piling up copies");

    // New: a second collection under a name that is taken gets a number, and nothing is replaced.
    Command({{"action","importInspect"},{"path",AutoRoute::Utf8Text(collectionBundle)}});
    Command({{"action","importApply"},{"path",AutoRoute::Utf8Text(collectionBundle)},{"mode","collectionNew"}});
    {
        const auto copyId=currentCollection();
        Check(copyId!=originId&&collectionRow(copyId).value("name",std::string{})=="来源 (2)",
            "importing a same-name collection without overwriting makes a numbered one instead");
        Check(routeCount(copyId)==2&&routeCount(originId)==2,
            "the numbered collection gets the routes and the original keeps its own");
        Check(rowCollection("shared-x")==originId,
            "the routes that were already here are not moved by importing a copy of them");
    }

    // A hand-picked bundle goes into the collection the player is in, and collisions are answered
    // rather than silently overwriting something unrelated.
    Command({{"action","export"},{"path",AutoRoute::Utf8Text(routesBundle)},
        {"routeIds",Json::array({"shared-x","shared-y"})}});
    Command({{"action","collectionCurrent"},{"collectionId",AutoRoute::DefaultCollectionId}});
    Command({{"action","importInspect"},{"path",AutoRoute::Utf8Text(routesBundle)}});
    Check(transfer().value("kind",std::string{})=="routes"&&transfer().at("conflict").is_null()&&
        transfer().value("collectionName",std::string{}).empty(),
        "a bundle of picked routes carries no collection and has nothing to collide with");
    Check(message().find("默认合集")!=std::string::npos,
        "the inspection says which collection the routes are about to join");
    Command({{"action","importApply"},{"path",AutoRoute::Utf8Text(routesBundle)},{"mode","routes"}});
    Check(currentCollection()==AutoRoute::DefaultCollectionId,
        "importing routes does not move the player out of the collection they are in");
    Check(rowCollection("shared-x")==originId&&rowName("shared-x")=="共享甲",
        "an id that is already taken here belongs to the route that had it, not to the newcomer");
    {
        const auto imported=routeIdIn("共享甲",AutoRoute::DefaultCollectionId);
        Check(!imported.empty()&&imported!="shared-x",
            "an imported route whose name is free in the target collection keeps it, under a fresh id");
        Check(rowCollection("shared-x")==originId,
            "the route that already held the id is the one it stays with");
    }
    Check(routeIdIn("共享甲 (2)",AutoRoute::DefaultCollectionId).empty(),
        "a name that is not taken in the collection is not numbered");
    // Importing the same picked routes a second time is the same answer again: the ids are taken and
    // so is the name, so this time both are answered.
    Command({{"action","importInspect"},{"path",AutoRoute::Utf8Text(routesBundle)}});
    Command({{"action","importApply"},{"path",AutoRoute::Utf8Text(routesBundle)},{"mode","routes"}});
    Check(!routeIdIn("共享甲 (2)",AutoRoute::DefaultCollectionId).empty(),
        "importing the same picked routes twice numbers the second copy instead of overwriting");

    // A package whose routes cannot be read is refused as a whole, and the collections do not move.
    {
        Json document;
        {
            std::ifstream input(routesBundle,std::ios::binary);
            document=Json::parse(std::string(std::istreambuf_iterator<char>(input),{}));
        }
        // Every route, so the package really has nothing importable in it.
        for(auto& route:document["routes"])route["stops"][0]["pointId"]="not-a-point";
        WriteTextAtomically(brokenBundle,document.dump(2));
    }
    const auto idsNow=savedIds();
    Check(refused({{"action","importInspect"},{"path",AutoRoute::Utf8Text(brokenBundle)}}),
        "a bundle whose only route cannot be resolved is refused at inspection");
    Check(refused({{"action","importApply"},{"path",AutoRoute::Utf8Text(brokenBundle)},{"mode","routes"}}),
        "and it is refused again if it is applied anyway");
    Check(savedIds()==idsNow&&currentCollection()==AutoRoute::DefaultCollectionId,
        "a bundle that cannot be imported changes nothing at all");
    Check(transfer().is_null(),"a refused inspection leaves no pending import behind");
}

// The route list is the one place a point type is described to the shell. The shell cannot resolve
// it by itself — the filter catalogue it holds knows far fewer identifiers than routes actually use
// — so the core has to send both the game's name and the resolved icon path.
void VerifyRouteListShape(){
    const auto listed=Command({{"action","list"}});
    const auto& rows=listed.at("data").at("savedRoutes");
    Check(rows.is_array()&&!rows.empty(),"the route list reports the routes that were saved");
    std::size_t described=0;
    for(const auto& row:rows){
        if(!row.contains("stopCount")||!row.at("stopCount").is_number()){
            Check(false,"a listed route reports how many points it visits");return;
        }
        if(!row.contains("kinds")||!row.at("kinds").is_array()){
            Check(false,"a listed route carries a kinds array");return;
        }
        for(const auto& kind:row.at("kinds")){
            if(!kind.contains("nameId")||!kind.contains("name")||!kind.contains("icon")){
                Check(false,"each listed point type carries nameId, name and icon");return;
            }
            ++described;
        }
    }
    Check(true,"the route list describes its point types with a name and an icon path");
    // The fixture's route is built from catalogue points, so the descriptors must be non-empty: a
    // silently empty list here is exactly the bug this check exists for.
    Check(described>0,"the route list actually described the fixture's point types");
    if(!described)std::cerr<<"note: fixture has no catalogue point types to describe\n";
}

// Collections are the route list's own organisation: which bucket a route is filed under, which
// bucket a route that has never been written lands in, and what deleting a collection does to the
// routes inside it. It installs its own point catalogue and restarts the service, because it writes
// routes of its own and must not depend on which fixture the block before it left behind — the
// farming block swaps the catalogue out from under everything that ran before it.
void VerifyCollections(){
    RoutePlanningService::Shutdown();
    AutoRoute::RouteCollections index(StructuredLogger::root/"SavedRoutes");
    const auto* scene=Scene::Find(1);
    const auto state=std::to_string(scene->kuroStateId);
    Json points=Json::array();
    for(const auto& pair:std::vector<std::pair<std::string,double>>{{"alpha",100},{"beta",200},{"gamma",300}})
        points.push_back({{"id",pair.first},{"x",pair.second/scene->scale*100},{"y",0},{"stateId",scene->kuroStateId}});
    DrawItemBase::itemsJsonData_World=Json::array({{{"id","chest"},{"name","test"},{"location",points}}});
    DrawItemBase::completed.clear();
    DrawItemBase::refreshablePointIds.clear();
    DrawItemBase::refreshableCategories.clear();
    RoutePlanningService::Initialize();
    AutoRoute::RoutePlanStore store(StructuredLogger::root/"SavedRoutes"/"Auto");
    const auto refused=[&](Json command){
        const auto result=RoutePlanningService::Command(command);
        return !result.value("accepted",false);
    };
    // Every helper holds the snapshot it read. `at()` hands back a reference into the object it was
    // called on, so ranging over `Snapshot().at(...)` directly would walk a destroyed temporary.
    const auto state_=[&](){return RoutePlanningService::Snapshot();};
    const auto collections=[&](){return state_().at("collections");};
    const auto collectionRow=[&](const std::string& id)->Json{
        const auto rows=collections();
        for(const auto& row:rows)if(row.value("id",std::string{})==id)return row;
        return Json(nullptr);
    };
    const auto currentCollection=[&](){return state_().value("currentCollection",std::string{});};
    const auto routeCount=[&](const std::string& id){return collectionRow(id).at("routeCount").get<int>();};
    const auto savedIdByName=[&](const std::string& name)->std::string{
        const auto rows=state_().at("savedRoutes");
        for(const auto& row:rows)if(row.value("name",std::string{})==name)return row.value("id",std::string{});
        return {};
    };
    const auto rowCollection=[&](const std::string& routeId)->std::string{
        const auto rows=state_().at("savedRoutes");
        for(const auto& row:rows)if(row.value("id",std::string{})==routeId)return row.value("collection",std::string{});
        return "";
    };

    // The template every route in this block is cloned from. It is written by the service itself, so
    // it resolves against the catalogue installed just above.
    Command({{"action","new"},{"sceneId",1}});
    Command({{"action","setStart"},{"sceneId",1},{"x",100},{"y",0}});
    {
        std::vector<std::string> keys;for(const auto* id:{"alpha","beta","gamma"})keys.push_back(state+":"+id);
        Command({{"action","add"},{"keys",keys}});
    }
    Command({{"action","generate"}});
    {
        const auto deadline=Clock::now()+3s;
        while(!RoutePlanningService::View().preview&&Clock::now()<deadline)std::this_thread::sleep_for(10ms);
    }
    Command({{"action","activate"}});
    const auto template_=*RoutePlanningService::View().active;
    Command({{"action","stop"}});
    const auto write=[&](const std::string& id,const std::string& name,const std::string& collection){
        auto plan=template_;plan.id=id;plan.name=name;plan.collection=collection;
        plan.skipped.clear();plan.skipHistory.clear();store.Save(plan,false);
    };
    // A route id identifies one route but not which folder holds it, so the path has to say.
    const auto routePath=[&](const std::string& id,bool handDrawn=false){
        return StructuredLogger::root/"SavedRoutes"/(handDrawn?"Hand":"Auto")/"local"/(id+".json");};

    Command({{"action","list"}});
    Check(collections().size()==1&&collectionRow(AutoRoute::DefaultCollectionId).is_object(),
        "the default collection is the whole list until the player creates one");
    Check(collectionRow(AutoRoute::DefaultCollectionId).value("system",false)&&
        collectionRow(AutoRoute::DefaultCollectionId).value("current",false),
        "the default collection is marked as the system one and starts out current");
    Check(AutoRoute::RouteCollections::Find(index.Load("local"),AutoRoute::DefaultCollectionId)==nullptr,
        "the default collection is never written into the index file");

    Command({{"action","collectionNew"},{"name","  宝箱路线  "}});
    const auto chestId=currentCollection();
    Check(chestId!=AutoRoute::DefaultCollectionId&&collections().size()==2,
        "creating a collection adds it to the list");
    Check(collectionRow(chestId).value("name",std::string{})=="宝箱路线"&&collectionRow(chestId).value("current",false),
        "a collection name is trimmed, and creating one also enters it");
    Check(routeCount(chestId)==0,"a new collection starts empty");
    Check(!collectionRow(chestId).value("system",true),"a collection the player created is not a system one");
    Check(index.Load("local").current==chestId,"entering a new collection is written to the index, not only held in memory");

    Check(refused({{"action","collectionNew"},{"name","宝箱路线"}}),
        "creating a collection whose name is taken is refused rather than silently numbered");
    Check(refused({{"action","collectionNew"},{"name",AutoRoute::DefaultCollectionName}}),
        "the default collection's name is reserved");
    Check(refused({{"action","collectionNew"},{"name","   "}}),
        "a collection name cannot be blank");
    Check(refused({{"action","collectionNew"},{"name",std::string(41,'a')}}),
        "a collection name cannot be longer than the row that shows it");
    Check(refused({{"action","collectionNew"},{"name","bad\nname"}}),
        "a collection name cannot carry a control character");
    Check(currentCollection()==chestId&&collections().size()==2,
        "a refused collection leaves both the list and the current collection as they were");

    // A route written before collections existed already belongs to the default collection, and a
    // route whose file names a collection the index does not know is shown under it too, rather
    // than disappearing from every list at once.
    write("ghost-route","幽灵路线","no-such-collection");
    Command({{"action","list"}});
    Check(rowCollection("ghost-route")==AutoRoute::DefaultCollectionId,
        "a route naming a collection that does not exist is shown under the default collection");
    Check(routeCount(AutoRoute::DefaultCollectionId)>=1,
        "the collection counts match the rows the list is showing");

    // A drawing and a fresh preview both land in the collection the player is in *at save time*,
    // which is the whole reason the collection can be switched at all.
    RoutePlanningService::ObserveMap(1,{});
    Command({{"action","handStart"},{"category","collectible"}});
    Command({{"action","handPoint"},{"x",7},{"y",0}});
    Command({{"action","handPoint"},{"x",9},{"y",0}});
    Command({{"action","handCommit"},{"name","手绘进合集"}});
    const auto handId=savedIdByName("手绘进合集");
    Check(!handId.empty()&&rowCollection(handId)==chestId,
        "a hand-drawn route is saved into the collection the player is currently in");
    Check(routeCount(chestId)>=1,"the drawing is counted in that collection");
    {
        const auto document=[&]{std::ifstream input(routePath(handId,true));return Json::parse(input);}();
        Check(document.value("collection",std::string{})==chestId&&document.value("handDrawn",false),
            "the collection is written into the route file, so it survives a restart");
    }

    Command({{"action","collectionCurrent"},{"collectionId",AutoRoute::DefaultCollectionId}});
    Check(currentCollection()==AutoRoute::DefaultCollectionId,"switching back to the default collection is reported");
    write("keep-a","保持原位",AutoRoute::DefaultCollectionId);
    write("keep-b","也保持原位",chestId);
    // A route file written before collections existed has no collection key at all. Removing it by
    // hand is the only honest way to test that, because Save always writes one.
    {
        auto document=[&]{std::ifstream input(routePath("keep-a"));return Json::parse(input);}();
        document.erase("collection");
        WriteTextAtomically(routePath("keep-a"),document.dump(2));
    }
    Command({{"action","list"}});
    Check(rowCollection("keep-a")==AutoRoute::DefaultCollectionId&&rowCollection("keep-b")==chestId,
        "a route file with no collection field at all reads as the default collection");

    // Moving is the only way a route changes collection after it is written. The active route must
    // be somewhere else entirely, because that is the case the pre-action fence guards.
    Command({{"action","load"},{"routeId","keep-b"}});
    RoutePlanningService::ObserveMap(1,{});
    Command({{"action","resume"}});
    Check(RoutePlanningService::View().active&&RoutePlanningService::View().active->id=="keep-b",
        "fixture must be following keep-b while another route is moved");
    Check(refused({{"action","state"},{"routeId","keep-a"}}),
        "the active-route fence refuses a route named with the singular routeId");
    Check(!refused({{"action","routeCollection"},{"routeIds",Json::array({"keep-a"})},
        {"collectionId",chestId}}),
        "a batch move names its routes with routeIds, so the active-route fence never sees them");
    Check(rowCollection("keep-a")==chestId,"moving a route rewrites which collection its file names");
    Check(RoutePlanningService::View().active->id=="keep-b"&&rowCollection("keep-b")==chestId,
        "moving another route neither switches nor moves the active one");
    Check(store.Load("local","keep-a",[&](int scene,const std::string& key)->std::optional<ItemDatas>{
        if(scene!=template_.sceneId)return {};for(const auto& item:template_.stops)if(AutoRoute::Key(item)==key)return item;return {};}).name=="保持原位",
        "moving a route changes nothing but its collection");
    Check(refused({{"action","routeCollection"},{"routeIds",Json::array({"keep-a"})},{"collectionId","gone"}}),
        "moving into a collection that does not exist is refused");
    Check(refused({{"action","routeCollection"},{"routeIds",Json::array()},{"collectionId",chestId}}),
        "moving nothing is refused rather than reported as a success");
    Command({{"action","routeCollection"},{"routeIds",Json::array({"keep-b"})},
        {"collectionId",AutoRoute::DefaultCollectionId}});
    Check(rowCollection("keep-b")==AutoRoute::DefaultCollectionId&&
        RoutePlanningService::View().active->collection==AutoRoute::DefaultCollectionId,
        "moving the route being followed keeps the in-memory copy in step with the file");

    // Saving an already-saved route rewrites its own file and leaves it where it is: looking at
    // another collection is not a request to move the player's work.
    Command({{"action","collectionCurrent"},{"collectionId",chestId}});
    Command({{"action","save"},{"name","改过名字的路线"}});
    Check(rowCollection("keep-b")==AutoRoute::DefaultCollectionId&&
        RoutePlanningService::View().active->collection==AutoRoute::DefaultCollectionId,
        "saving an already-saved route keeps its own collection instead of the current one");
    Command({{"action","collectionCurrent"},{"collectionId",AutoRoute::DefaultCollectionId}});

    // Deleting a collection takes its routes with it — the rule the player chose — and everything
    // that was pointing at one of them has to let go.
    Command({{"action","routeCollection"},{"routeIds",Json::array({"keep-b"})},{"collectionId",chestId}});
    Command({{"action","load"},{"routeId","keep-b"}});
    RoutePlanningService::ObserveMap(1,{});
    Command({{"action","resume"}});
    Check(RoutePlanningService::View().active&&RoutePlanningService::View().active->id=="keep-b",
        "fixture must be following a route inside the collection that is about to be deleted");
    const auto completionState=template_.stops.front().layer.stateId;
    const auto deletedFree=std::to_string(completionState)+":free-route:keep-b:free:1";
    const auto retainedFree=std::to_string(completionState)+":free-route:ghost-route:free:1";
    const auto official=AutoRoute::Key(template_.stops.front());
    {std::scoped_lock lock(DrawItemBase::mutex);DrawItemBase::completed.insert(deletedFree);DrawItemBase::completed.insert(retainedFree);DrawItemBase::completed.insert(official);}
    Command({{"action","collectionDelete"},{"collectionId",chestId}});
    {std::scoped_lock lock(DrawItemBase::mutex);Check(!DrawItemBase::completed.contains(deletedFree)&&DrawItemBase::completed.contains(retainedFree)&&DrawItemBase::completed.contains(official),"collection deletion clears scoped free progress but preserves other routes and official points");}
    Check(!std::filesystem::exists(routePath("keep-b"))&&!std::filesystem::exists(routePath("keep-a")),
        "deleting a collection deletes the routes that were filed in it");
    Check(rowCollection("ghost-route")==AutoRoute::DefaultCollectionId,
        "deleting a collection leaves every other route alone");
    Check(RoutePlanningService::View().active==std::nullopt,
        "deleting a collection that held the route being followed ends that navigation");
    Check(currentCollection()==AutoRoute::DefaultCollectionId&&collections().size()==1,
        "the deleted collection is gone and the player is back in the default one");
    Check(index.Load("local").collections.empty(),"the deleted collection is gone from the index file too");
    Check(!std::filesystem::exists(routePath("keep-b").string()+".deleting"),
        "deleting a collection leaves no half-deleted route behind");

    Check(refused({{"action","collectionDelete"},{"collectionId",AutoRoute::DefaultCollectionId}}),
        "the default collection cannot be deleted");
    Check(refused({{"action","collectionRename"},{"collectionId",AutoRoute::DefaultCollectionId},{"name","改名"}}),
        "the default collection cannot be renamed");
    Check(refused({{"action","collectionRename"},{"collectionId","no-such-collection"},{"name","改名"}}),
        "renaming a collection that does not exist is refused");
    Check(refused({{"action","collectionCurrent"},{"collectionId","no-such-collection"}}),
        "switching to a collection that does not exist is refused");
    Check(refused({{"action","collectionDelete"},{"collectionId","no-such-collection"}}),
        "deleting a collection that does not exist is refused");

    Command({{"action","collectionNew"},{"name","采集"}});
    const auto herbId=currentCollection();
    Command({{"action","collectionRename"},{"collectionId",herbId},{"name","采集路线"}});
    Check(collectionRow(herbId).value("name",std::string{})=="采集路线",
        "a collection can be renamed, and the new name is what the list reports");
    Check(refused({{"action","collectionRename"},{"collectionId",herbId},{"name",AutoRoute::DefaultCollectionName}}),
        "renaming onto the reserved name is refused");
    Command({{"action","collectionRename"},{"collectionId",herbId},{"name","采集路线"}});
    Check(currentCollection()==herbId,"renaming does not change which collection the player is in");
    Command({{"action","collectionCurrent"},{"collectionId",AutoRoute::DefaultCollectionId}});
    // The index is the authority on restart: whatever it says is where the player comes back to.
    Check(index.Load("local").current==AutoRoute::DefaultCollectionId&&index.Load("local").collections.size()==1,
        "the index on disk holds exactly the collections that survived, in creation order");
}
}
int main(int argc,char** argv){
    StructuredLogger::root=std::filesystem::absolute(argc>1?argv[1]:"out/auto-replan-native/service-data");
    std::filesystem::create_directories(StructuredLogger::root);
    // The harness owns this directory and may be handed one an earlier run used — the gate reuses
    // out\system-audit\route-service-data. Its route tree is cleared so every run starts from the
    // same state: the service reads the collections index once per process, before any test can
    // reset it, and a leftover collection or a leftover route name would otherwise be read as a
    // change in behaviour rather than as yesterday's data.
    {
        std::error_code ignored;
        std::filesystem::remove_all(StructuredLogger::root/"SavedRoutes",ignored);
    }
    const bool failSave=argc>2&&std::string(argv[2])=="save-failure";
    try{
        const auto original=Prepare();const auto originalIds=Ids(original);const auto oldKey=AutoRoute::Key(original.stops.front());
        if(argc>2&&std::string(argv[2])=="map-suspension"){
            VerifyMapSuspension(original);
            RoutePlanningService::Shutdown();
            std::cout<<"Map suspension harness failures="<<failures<<'\n';return failures?1:0;
        }
        Pump([]{return false;},400ms);
        Check(AutoRoute::Key(RoutePlanningService::View().active->stops.front())==oldKey,"default-off service never reorders from observations");
        HANDLE held=INVALID_HANDLE_VALUE;
        if(failSave){const auto path=StructuredLogger::root/"SavedRoutes"/"Auto"/"local"/(original.id+".json");
            held=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            Check(held!=INVALID_HANDLE_VALUE,"test holds saved route against atomic replacement");}
        const auto enabledAt=Clock::now();RoutePlanningService::SetAutoReplanEnabled(true);
        Check(Pump([&]{const auto v=RoutePlanningService::View();return failSave?v.autoReplanStatus=="saveFailed":v.previousTarget.has_value();},9s),
            "real worker reaches a decision from fresh 80ms observations after enabling");
        auto current=RoutePlanningService::View();
        Check(Clock::now()-enabledAt>=2s,"real target change cannot bypass the second-solve confirmation interval");
        if(failSave){
            Check(AutoRoute::SameOrder(current.active->stops,original.stops)&&current.active->skipped==original.skipped&&
                !current.previousTarget,"failed atomic save preserves the old order, target and hint state");
            if(held!=INVALID_HANDLE_VALUE)CloseHandle(held);
            Check(Pump([]{return RoutePlanningService::View().previousTarget.has_value();},6s),"service retries current observations after storage becomes writable");
            current=RoutePlanningService::View();
        }
        Check(current.active&&Ids(*current.active)==originalIds&&current.active->skipHistory==original.skipHistory&&
            current.active->skipped==original.skipped,"real automatic application preserves all IDs and skipped undo history");
        // Every read of the current target is checked before it is indexed. A harness that dereferences
        // a target which is not there does not fail — it dies with an access violation and prints
        // nothing at all, so a whole run becomes "exit code 0xC0000005" with no clue how far it got.
        // (Found 2026-10-01 while releasing: the atomic-commit retry shifted the timing enough to reach
        // a state this line assumed was impossible.)
        if(!current.active||current.currentTargetIndex<0)
            throw std::runtime_error("real worker left no current target to compare with");
        Check(current.previousTarget&&AutoRoute::Key(*current.previousTarget)==oldKey&&
            current.active->stops[current.currentTargetIndex].itemId=="second","real worker selects new target and retains exactly the former one");
        AutoRoute::RoutePlanStore store(StructuredLogger::root/"SavedRoutes"/"Auto");
        const auto saved=store.Load("local",original.id,[&](int scene,const std::string& key)->std::optional<ItemDatas>{
            if(scene==original.sceneId)for(const auto& item:original.stops)if(AutoRoute::Key(item)==key)return item;return {};});
        if(!current.active)throw std::runtime_error("route disappeared before the saved file could be compared");
        Check(AutoRoute::SameOrder(saved.stops,current.active->stops)&&Ids(saved)==originalIds&&saved.skipHistory==original.skipHistory,
            "accepted automatic order survives loading the actual route file with every skipped and completed member");
        ItemDatas unrelated;unrelated.itemId="unrelated";unrelated.layer.stateId=Scene::Find(1)->kuroStateId;
        Complete(unrelated,true);
        Check(RoutePlanningService::View().previousTarget.has_value(),"unrelated marker completion leaves the comparison intact");
        const auto completedBefore=RoutePlanningService::View().completed;
        Check(Pump([]{return !RoutePlanningService::View().previousTarget;},1500ms,0),"sustained nearby observations erase the dashed hint");
        Check(RoutePlanningService::View().completed==completedBefore,"nearby confirmation never completes the new target");
        RoutePlanningService::SetAutoReplanEnabled(false);
        const auto settled=RoutePlanningService::View();
        if(!settled.active||settled.currentTargetIndex<0)
            throw std::runtime_error("reordered route has no current target left to complete");
        const auto target=settled.active->stops[settled.currentTargetIndex];
        Complete(target,true);Complete(target,false);current=RoutePlanningService::View();
        if(!current.active)throw std::runtime_error("completing and cancelling removed the whole route");
        Check(Ids(*current.active)==originalIds&&current.active->skipHistory==original.skipHistory&&!current.completed.contains(AutoRoute::Key(target)),
            "completion and cancellation keep all route members and skipped undo history");
        Command({{"action","undoSkip"}});
        Check(RoutePlanningService::View().active&&RoutePlanningService::View().active->skipped.empty(),
            "original skip can still be undone after automatic reordering");
        VerifyViewportSelection(original);
        VerifyGuideSkipGuard(original);
        VerifyRouteToolbarNavigation();
        VerifyToolbarLayoutNavigation();
        VerifyFarmMode();
        VerifyRouteListShape();
        VerifyCollections();
        VerifyRouteBundle();
        VerifyTypedFreeRoutes(original);
    }catch(const std::exception& e){++failures;std::cerr<<"UNEXPECTED: "<<e.what()<<'\n';}
    RoutePlanningService::Shutdown();
    std::cout<<"RoutePlanningService harness failures="<<failures<<'\n';return failures?1:0;
}
