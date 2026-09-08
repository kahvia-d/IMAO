#include "RoutePlanningServiceTestHost.h"
#include "Runtime/RoutePlanStore.h"
#include "Runtime/RouteViewportCandidates.h"
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
        result.Add(unobscured,exposedPosition,width,height,false,false);
        return result;
    };
    const auto covered=build(true),uncovered=build(false);
    const std::vector<ItemDatas> expected{unobscured,behindTools};
    const auto expectedKeys=SelectionKeys(expected);
    Check(SelectionKeys(covered.inViewport)==expectedKeys,
        "viewport candidates keep the obscured marker but exclude every offscreen, nonfinite, completed and duplicate input");
    Check(SelectionKeys(uncovered.inViewport)==expectedKeys&&
        SelectionKeys(covered.inViewport)==SelectionKeys(uncovered.inViewport),
        "opening or closing tools cannot change bulk viewport candidate identity");
    Check(covered.onCanvas.size()==1&&AutoRoute::Key(covered.onCanvas.front())==AutoRoute::Key(unobscured)&&
        covered.canvasPositions.size()==1&&covered.canvasPositions.front().x==exposedPosition.x&&covered.canvasPositions.front().y==exposedPosition.y,
        "covered markers stay out of direct canvas gestures and point-position vectors remain aligned");
    Check(SelectionKeys(uncovered.onCanvas)==expectedKeys&&uncovered.canvasPositions.size()==2&&
        uncovered.canvasPositions[0].x==exposedPosition.x&&uncovered.canvasPositions[0].y==exposedPosition.y&&
        uncovered.canvasPositions[1].x==coveredPosition.x&&uncovered.canvasPositions[1].y==coveredPosition.y,
        "closing the tools restores both gesture candidates with their original aligned positions");
    AutoRoute::ViewportCandidates coincident;
    coincident.Add(unobscured,exposedPosition,width,height,false,false);
    coincident.Add(behindTools,exposedPosition,width,height,false,false);
    Check(SelectionKeys(coincident.inViewport)==expectedKeys&&SelectionKeys(coincident.onCanvas)==expectedKeys&&
        coincident.canvasPositions.size()==2&&coincident.canvasPositions[0].x==coincident.canvasPositions[1].x&&
        coincident.canvasPositions[0].y==coincident.canvasPositions[1].y,
        "same-coordinate markers retain both distinct identities in bulk and gesture candidate sets");

    Command({{"action","new"},{"sceneId",source.sceneId}});
    RoutePlanningService::ObserveMap(source.sceneId,covered.inViewport);
    const auto addVisible=[](){
        const auto view=RoutePlanningService::View();
        return Command({{"action","addVisible"},{"profileId",view.profileId},
            {"expectedSceneId",view.sceneId},{"expectedGeneration",view.generation},{"expectedRevision",view.revision}});
    };
    addVisible();
    auto selected=RoutePlanningService::View();
    Check(SelectionKeys(selected.selected)==expectedKeys&&selected.hiddenCount==0,
        "ObserveMap to real addVisible selects the tools-covered identity without adding offscreen or completed markers");
    addVisible();
    Check(SelectionKeys(RoutePlanningService::View().selected)==expectedKeys,
        "repeated addVisible does not duplicate either overlapping identity");
    RoutePlanningService::ObserveMap(source.sceneId,uncovered.inViewport);
    Check(RoutePlanningService::View().hiddenCount==0&&SelectionKeys(RoutePlanningService::View().selected)==expectedKeys,
        "collapsing the tools does not make previously selected viewport points become hidden or alter the draft");
    Command({{"action","undo"}});
    Check(RoutePlanningService::View().selected.empty(),
        "one undo reverses the entire viewport append even after a duplicate append and tools collapse");

    // Preserve a previously selected offscreen member, so hidden-count stability
    // and undo are checked against a nonempty prior draft as well.
    Command({{"action","add"},{"keys",std::vector<std::string>{AutoRoute::Key(outside)}}});
    const auto before=RoutePlanningService::View().selected;
    RoutePlanningService::ObserveMap(source.sceneId,covered.inViewport);
    Check(RoutePlanningService::View().hiddenCount==1,"an existing genuinely offscreen selection is counted as hidden");
    addVisible();
    auto combined=expected;combined.push_back(outside);
    Check(SelectionKeys(RoutePlanningService::View().selected)==SelectionKeys(combined)&&RoutePlanningService::View().hiddenCount==1,
        "bulk append retains the previous offscreen selection and does not count tools coverage as hidden");
    RoutePlanningService::ObserveMap(source.sceneId,uncovered.inViewport);
    Check(RoutePlanningService::View().hiddenCount==1,
        "removing tools coverage leaves the true offscreen hidden count unchanged");
    Command({{"action","undo"}});
    selected=RoutePlanningService::View();
    Check(SelectionKeys(selected.selected)==SelectionKeys(before)&&selected.selected.size()==1&&
        selected.selected.front().itemMapROC.x==outside.itemMapROC.x&&
        selected.selected.front().itemMapROC.y==outside.itemMapROC.y&&selected.hiddenCount==1,
        "undo restores the exact nonempty prior selection and its original coordinates and hidden count");
    Check(activeBefore&&selected.active&&selected.active->id==activeBefore->id&&
        AutoRoute::SameOrder(selected.active->stops,activeBefore->stops)&&selected.active->skipHistory==activeBefore->skipHistory,
        "viewport-only draft regression leaves the previously tested active route and undo-skip history unchanged");
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
    }catch(const std::exception& e){++failures;std::cerr<<"UNEXPECTED: "<<e.what()<<'\n';}
    RoutePlanningService::Shutdown();
    std::cout<<"RoutePlanningService harness failures="<<failures<<'\n';return failures?1:0;
}
