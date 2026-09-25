#include "RoutePlanningServiceTestHost.h"
#include "Runtime/RoutePlanStore.h"
#include "Runtime/RouteViewportCandidates.h"
#include "Runtime/RouteToolbarNavigation.h"
#include <algorithm>
#include <iostream>
#include <limits>
#include <thread>

using namespace std::chrono_literals;
using Clock=AutoRoute::ReplanClock;
using Json=nlohmann::json;
namespace {
int failures=0;std::uint64_t sequence=0;
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
}
int main(int argc,char** argv){
    StructuredLogger::root=std::filesystem::absolute(argc>1?argv[1]:"out/auto-replan-native/service-data");
    std::filesystem::create_directories(StructuredLogger::root);
    const bool failSave=argc>2&&std::string(argv[2])=="save-failure";
    try{
        const auto original=Prepare();const auto originalIds=Ids(original);const auto oldKey=AutoRoute::Key(original.stops.front());
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
        Check(current.previousTarget&&AutoRoute::Key(*current.previousTarget)==oldKey&&
            current.active->stops[current.currentTargetIndex].itemId=="second","real worker selects new target and retains exactly the former one");
        AutoRoute::RoutePlanStore store(StructuredLogger::root/"SavedRoutes"/"Auto");
        const auto saved=store.Load("local",original.id,[&](int scene,const std::string& key)->std::optional<ItemDatas>{
            if(scene==original.sceneId)for(const auto& item:original.stops)if(AutoRoute::Key(item)==key)return item;return {};});
        Check(AutoRoute::SameOrder(saved.stops,current.active->stops)&&Ids(saved)==originalIds&&saved.skipHistory==original.skipHistory,
            "accepted automatic order survives loading the actual route file with every skipped and completed member");
        ItemDatas unrelated;unrelated.itemId="unrelated";unrelated.layer.stateId=Scene::Find(1)->kuroStateId;
        Complete(unrelated,true);
        Check(RoutePlanningService::View().previousTarget.has_value(),"unrelated marker completion leaves the comparison intact");
        const auto completedBefore=RoutePlanningService::View().completed;
        Check(Pump([]{return !RoutePlanningService::View().previousTarget;},1500ms,0),"sustained nearby observations erase the dashed hint");
        Check(RoutePlanningService::View().completed==completedBefore,"nearby confirmation never completes the new target");
        RoutePlanningService::SetAutoReplanEnabled(false);
        const auto target=RoutePlanningService::View().active->stops[RoutePlanningService::View().currentTargetIndex];
        Complete(target,true);Complete(target,false);current=RoutePlanningService::View();
        Check(Ids(*current.active)==originalIds&&current.active->skipHistory==original.skipHistory&&!current.completed.contains(AutoRoute::Key(target)),
            "completion and cancellation keep all route members and skipped undo history");
        Command({{"action","undoSkip"}});
        Check(RoutePlanningService::View().active->skipped.empty(),"original skip can still be undone after automatic reordering");
        VerifyViewportSelection(original);
        VerifyGuideSkipGuard(original);
        VerifyRouteToolbarNavigation();
    }catch(const std::exception& e){++failures;std::cerr<<"UNEXPECTED: "<<e.what()<<'\n';}
    RoutePlanningService::Shutdown();
    std::cout<<"RoutePlanningService harness failures="<<failures<<'\n';return failures?1:0;
}
