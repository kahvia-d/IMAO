#include "RoutePlanningService.h"
#include "RoutePlanStore.h"
#ifdef IMAO_ROUTE_SERVICE_TEST
#include "../../tests/RoutePlanningServiceTestHost.h"
#else
#include "StructuredLogger.h"
#include "../ImguiDraw/Items/DrawItemBase.h"
#endif
#include "../Coordinate/KuroMapCoordinates.h"
#include "../Coordinate/locationCalculator/RelativeCoordinates.h"
#include <bcrypt.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace {
using Json=nlohmann::json;
using Clock=std::chrono::steady_clock;
struct Draft {
    AutoRoute::Start start;
    std::vector<ItemDatas> selected;
    std::vector<std::vector<ItemDatas>> history;
    std::optional<AutoRoute::Plan> preview;
};
struct Job {
    std::uint64_t epoch=0;
    std::string profile;
    int scene=0;
    AutoRoute::Start start;
    std::vector<ItemDatas> selected;
};
struct Runtime {
    std::mutex mutex,eventMutex;
    std::condition_variable wake;
    std::thread worker, autoWorker;
    std::condition_variable autoWake;
    bool ready=false,stopping=false,enabled=false,computing=false,pendingNew=false,runRequested=false,playerAvailable=false;
    std::atomic_uint64_t epoch{1};
    std::uint64_t revision=1;
    int scene=0,observedScene=0;
    std::string profile,tool="pan",message;
    std::map<int,Draft> drafts;
    std::unordered_map<int,std::unordered_map<std::string,ItemDatas>> catalog;
    std::unordered_map<std::string,std::string> names;
    std::vector<ItemDatas> visible;
    std::unordered_set<std::string> visibleKeys,completed;
    AutoRoute::Start mapStart,player;
    std::optional<AutoRoute::Plan> active;
    std::vector<std::string> skipHistory;
    std::optional<Job> pending;
    std::unique_ptr<AutoRoute::RoutePlanStore> store;
    Json saved=Json::array();
    std::function<void(const Json&)> callback;
    Clock::time_point lastVisibilityEvent{};
    bool visibilityEventPending=false;
    bool autoEnabled=false, autoComputing=false, autoDirty=true;
    std::string autoStatus="disabled", candidateTarget;
    std::atomic_uint64_t autoEpoch{1};
    std::uint64_t orderRevision=0;
    AutoRoute::StablePlayer stablePlayer;
    AutoRoute::ProximityObservation proximity;
    AutoRoute::NearbyConfirmation nearConfirmation;
    std::optional<ItemDatas> previousTarget;
    Clock::time_point lastAutoSolve{}, lastTargetSwitch{}, candidateSince{};
    Coordinate lastAutoPosition;
    bool hasAutoPosition=false;
    std::unordered_set<std::string> spacingKeys;
    double spacing=0;
};
Runtime& R(){static Runtime state;return state;}
bool Finite(const Coordinate& p){return std::isfinite(p.x)&&std::isfinite(p.y);}
std::string NewId(){
    std::array<unsigned char,16> bytes{};
    if(BCryptGenRandom(nullptr,bytes.data(),static_cast<ULONG>(bytes.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)!=0)
        throw std::runtime_error("无法创建自动路线标识");
    bytes[6]=(bytes[6]&15)|64;bytes[8]=(bytes[8]&63)|128;
    constexpr char hex[]="0123456789abcdef";std::string id;
    for(std::size_t i=0;i<bytes.size();++i){if(i==4||i==6||i==8||i==10)id+='-';id+=hex[bytes[i]>>4];id+=hex[bytes[i]&15];}
    return id;
}
void InvalidateAutoLocked(bool clearComparison=false){
    auto& r=R();++r.autoEpoch;r.autoDirty=true;r.autoComputing=false;r.candidateTarget.clear();r.candidateSince={};
    if(clearComparison){r.previousTarget.reset();r.nearConfirmation.Reset();r.proximity={};++r.orderRevision;}
    r.autoWake.notify_all();
}
void InvalidateLocked(){auto& r=R();++r.epoch;r.pending.reset();r.computing=false;++r.revision;InvalidateAutoLocked();}
void StopNavigationLocked(){
    auto& r=R();InvalidateLocked();r.active.reset();r.skipHistory.clear();r.runRequested=false;
    InvalidateAutoLocked(true);
    r.enabled=false;r.pendingNew=false;r.tool="pan";
    for(auto& [scene,draft]:r.drafts)draft.preview.reset();
}
Draft& DraftLocked(){auto& r=R();if(!Scene::IsKnown(r.scene))throw std::runtime_error("请先打开大地图并完成识别");return r.drafts[r.scene];}
std::optional<ItemDatas> ResolveLocked(int scene,const std::string& key){
    const auto sceneIt=R().catalog.find(scene);if(sceneIt==R().catalog.end())return {};
    const auto it=sceneIt->second.find(key);return it==sceneIt->second.end()?std::optional<ItemDatas>{}:it->second;
}
bool CompletedNow(int scene,const ItemDatas& item){return DrawItemBase::IsPointCompleted(Scene::SceneIdToName(scene),item);}
void RefreshCompletedLocked(){
    auto& r=R();r.completed.clear();
    const auto add=[&](int scene,const std::vector<ItemDatas>& items){for(const auto& item:items)if(CompletedNow(scene,item))r.completed.insert(AutoRoute::Key(item));};
    for(const auto& [scene,draft]:r.drafts){add(scene,draft.selected);if(draft.preview)add(scene,draft.preview->stops);}
    if(r.active)add(r.active->sceneId,r.active->stops);
}
int TargetIndexLocked(){
    const auto& r=R();if(!r.active)return -1;
    for(std::size_t i=0;i<r.active->stops.size();++i){const auto key=AutoRoute::Key(r.active->stops[i]);
        if(!r.completed.contains(key)&&!r.active->skipped.contains(key))return static_cast<int>(i);}
    return -1;
}
std::string NavigationLocked(){
    const auto& r=R();if(!r.active)return "paused";
    if(TargetIndexLocked()<0)return "finished";
    if(!r.runRequested)return "paused";
    return r.playerAvailable&&r.player.valid&&r.player.sceneId==r.active->sceneId?"navigating":"waitingForLocation";
}
Json StopJsonLocked(const ItemDatas& item,int order=0,bool skipped=false){
    const auto found=R().names.find(item.nameId);
    return {{"key",AutoRoute::Key(item)},{"stateId",item.layer.stateId},{"pointId",item.itemId},{"nameId",item.nameId},
        {"name",found==R().names.end()?item.nameId:found->second},{"x",item.itemMapROC.x},{"y",item.itemMapROC.y},
        {"countryId",item.layer.countryId},{"floorId",item.layer.floorId},{"level",item.layer.level},
        {"completed",R().completed.contains(AutoRoute::Key(item))},{"skipped",skipped},{"order",order}};
}
Json PlanJsonLocked(const AutoRoute::Plan& plan){
    Json stops=Json::array();Coordinate previous=plan.start.roc;double length=0;
    for(std::size_t i=0;i<plan.stops.size();++i){const auto& item=plan.stops[i];
        stops.push_back(StopJsonLocked(item,static_cast<int>(i+1),plan.skipped.contains(AutoRoute::Key(item))));
        if(!R().completed.contains(AutoRoute::Key(item))&&!plan.skipped.contains(AutoRoute::Key(item))){
            length+=std::hypot(item.itemMapROC.x-previous.x,item.itemMapROC.y-previous.y);previous=item.itemMapROC;}}
    return {{"id",plan.id},{"name",plan.name},{"profileId",plan.profileId},{"sceneId",plan.sceneId},
        {"sceneName",Scene::SceneIdToName(plan.sceneId)},{"start",AutoRoute::StartJson(plan.start)},
        {"stops",std::move(stops)},{"planarLength",length}};
}
std::size_t HiddenLocked(){
    const auto& r=R();const auto it=r.drafts.find(r.scene);if(it==r.drafts.end())return 0;
    return std::count_if(it->second.selected.begin(),it->second.selected.end(),[&](const ItemDatas& p){return !r.visibleKeys.contains(AutoRoute::Key(p));});
}
Json SnapshotLocked(){
    const auto& r=R();Json selected=Json::array(),preview=nullptr;AutoRoute::Start start;
    const auto it=r.drafts.find(r.scene);
    if(it!=r.drafts.end()){start=it->second.start;for(const auto& p:it->second.selected)selected.push_back(StopJsonLocked(p));
        if(it->second.preview)preview=PlanJsonLocked(*it->second.preview);}
    const int target=TargetIndexLocked();
    return {{"profileId",r.profile},{"sceneId",r.scene},{"sceneName",Scene::SceneIdToName(r.scene)},
        {"enabled",r.enabled},{"tool",r.tool},{"computing",r.computing},{"revision",r.revision},{"generation",r.epoch.load()},
        {"message",r.message},{"selectedCount",selected.size()},{"hiddenCount",HiddenLocked()},
        {"start",AutoRoute::StartJson(start)},{"selected",std::move(selected)},{"preview",std::move(preview)},
        {"active",r.active?PlanJsonLocked(*r.active):Json(nullptr)},{"navigationStatus",NavigationLocked()},
        {"currentTarget",target>=0?StopJsonLocked(r.active->stops[target],target+1):Json(nullptr)},
        {"autoReplanEnabled",r.autoEnabled},{"autoReplanComputing",r.autoComputing},{"autoReplanStatus",r.autoStatus},
        {"orderRevision",r.orderRevision},{"previousTarget",r.previousTarget?StopJsonLocked(*r.previousTarget):Json(nullptr)},
        {"savedRoutes",r.saved}};
}
void Emit(){
    auto& r=R();std::function<void(const Json&)> callback;
    {std::scoped_lock lock(r.eventMutex);callback=r.callback;}
    if(!callback)return;
    Json state;{std::scoped_lock lock(r.mutex);state=SnapshotLocked();}
    callback({{"type","routePlanningChanged"},{"data",std::move(state)}});
}
void SyncProfileLocked(){
    auto& r=R();const auto profile=DrawItemBase::MarkerProfile();if(profile==r.profile)return;
    InvalidateLocked();r.profile=profile;r.enabled=false;r.pendingNew=false;r.drafts.clear();r.active.reset();r.skipHistory.clear();
    InvalidateAutoLocked(true);r.stablePlayer.Reset();r.hasAutoPosition=false;
    r.runRequested=false;r.completed.clear();r.message.clear();r.tool="pan";
    try {r.saved=r.store->List(profile);r.active=r.store->LoadActive(profile,ResolveLocked);
        if(r.active){r.skipHistory=r.active->skipHistory;r.message="已恢复自动路线，点击继续导航";}}
    catch(const std::exception& e){r.message=std::string("自动路线暂停：")+e.what();r.active.reset();}
    RefreshCompletedLocked();
}
void RememberSelectionLocked(Draft& draft){
    draft.history.push_back(draft.selected);if(draft.history.size()>100)draft.history.erase(draft.history.begin());
    draft.preview.reset();InvalidateLocked();
}
void AddLocked(const std::vector<std::string>& keys){
    auto& r=R();auto& draft=DraftLocked();std::vector<ItemDatas> added;std::unordered_set<std::string> present;
    for(const auto& p:draft.selected)present.insert(AutoRoute::Key(p));
    for(const auto& key:keys){
        const auto item=ResolveLocked(r.scene,key);if(!item)throw std::invalid_argument("点位不属于当前地图："+key);
        if(!CompletedNow(r.scene,*item)&&present.insert(key).second)added.push_back(*item);
    }
    if(draft.selected.size()+added.size()>AutoRoute::MaxTargets)throw std::invalid_argument("单条路线最多 500 点，本次追加未生效");
    if(!added.empty()){RememberSelectionLocked(draft);draft.selected.insert(draft.selected.end(),added.begin(),added.end());}
    RefreshCompletedLocked();r.message="已追加 "+std::to_string(added.size())+" 个目标";
}
void QueueSolveLocked(bool replan){
    auto& r=R();auto& draft=DraftLocked();
    if(replan){
        if(!r.active||r.active->sceneId!=r.scene)throw std::runtime_error("当前地图没有活动路线");
        auto start=r.observedScene==r.scene?r.mapStart:r.player;
        if(!start.valid||start.sceneId!=r.scene||(!r.observedScene&&!r.playerAvailable)) {
            if(draft.start.valid&&draft.start.sceneId==r.scene&&draft.start.source=="manual")start=draft.start;
            else throw std::runtime_error("无法取得当前位置，请重新定位或指定手动起点");
        }
        RememberSelectionLocked(draft);draft.start=start;draft.selected.clear();
        for(const auto& p:r.active->stops)if(!CompletedNow(r.scene,p)&&!r.active->skipped.contains(AutoRoute::Key(p)))draft.selected.push_back(p);
    }
    if(!draft.start.valid||draft.start.sceneId!=r.scene)throw std::runtime_error("起点未知，请使用“指定起点”或返回游戏重新定位");
    std::vector<ItemDatas> targets;for(const auto& p:draft.selected)if(!CompletedNow(r.scene,p))targets.push_back(p);
    if(targets.empty())throw std::runtime_error("请先选择至少一个未完成目标");
    if(targets.size()>AutoRoute::MaxTargets)throw std::runtime_error("单条路线最多 500 点");
    InvalidateLocked();draft.preview.reset();r.enabled=true;r.computing=true;r.message="正在优化访问顺序";
    r.pending=Job{r.epoch.load(),r.profile,r.scene,draft.start,std::move(targets)};r.wake.notify_one();
}
void Worker(){
    auto& r=R();for(;;){
        Job job;{std::unique_lock lock(r.mutex);r.wake.wait(lock,[&]{return r.stopping||r.pending.has_value();});
            if(r.stopping)return;job=std::move(*r.pending);r.pending.reset();}
        const auto begin=Clock::now();
        try {
            const auto result=AutoRoute::Solve(job.start,job.selected,[&]{return r.epoch.load()!=job.epoch;});
            {std::scoped_lock lock(r.mutex);
                if(result.cancelled||r.stopping||r.epoch.load()!=job.epoch||r.profile!=job.profile||r.scene!=job.scene)continue;
                r.computing=false;
                if(std::any_of(result.stops.begin(),result.stops.end(),[&](const ItemDatas& p){return CompletedNow(job.scene,p);}))
                    r.message="目标完成状态已变化，请重新生成路线";
                else {AutoRoute::Plan plan;plan.id=NewId();plan.name="自动路线";plan.profileId=job.profile;plan.sceneId=job.scene;
                    plan.start=job.start;plan.stops=result.stops;r.drafts[job.scene].preview=std::move(plan);r.message="路线预览已生成，点击开始导航";}
                ++r.revision;}
            StructuredLogger::Record("info","routes","auto-route-solved","targets="+std::to_string(job.selected.size())+
                " elapsedMs="+std::to_string(std::chrono::duration<double,std::milli>(Clock::now()-begin).count())+
                " initialLength="+std::to_string(result.initialLength)+" planarLength="+std::to_string(result.planarLength));Emit();
        }catch(const std::exception& e){
            {std::scoped_lock lock(r.mutex);if(r.epoch.load()!=job.epoch)continue;r.computing=false;r.message=std::string("规划失败：")+e.what();++r.revision;}Emit();
        }
    }
}
std::string AutoGateLocked(Clock::time_point now){
    const auto& r=R();
    if(!r.autoEnabled)return "disabled";
    if(!r.active||!r.runRequested||TargetIndexLocked()<0)return "paused";
    if(r.enabled||r.computing||r.pending||r.observedScene)return "editing";
    const auto& p=r.stablePlayer.last;
    if(!r.playerAvailable||!r.stablePlayer.Ready(now)||p.profileId!=r.profile||p.sceneId!=r.active->sceneId)return "waitingForLocation";
    return "ready";
}
bool ProximityValidLocked(Clock::time_point now){
    const auto& r=R();const auto& p=r.proximity;const int target=TargetIndexLocked();
    return p.valid&&r.active&&target>=0&&p.profileId==r.profile&&p.routeId==r.active->id&&
        p.orderRevision==r.orderRevision&&p.targetKey==AutoRoute::Key(r.active->stops[target])&&
        p.sceneId==r.active->sceneId&&p.sessionId==r.stablePlayer.last.sessionId&&
        now>=p.capturedAt&&now-p.capturedAt<=AutoRoute::CaptureLifetime&&now>=p.presentedAt&&
        now-p.presentedAt<std::chrono::milliseconds(100)&&std::isfinite(p.distancePixels);
}
void AutoWorker(){
    auto& r=R();
    for(;;){
        AutoRoute::Plan original;std::unordered_set<std::string> completed;
        AutoRoute::PlayerObservation player;std::vector<ItemDatas> remaining;
        std::uint64_t epoch=0,orderRevision=0;double spacing=0;bool calculate=false,statusChanged=false;
        {
            std::unique_lock lock(r.mutex);r.autoWake.wait_for(lock,std::chrono::milliseconds(500));
            if(r.stopping)return;
            const auto now=Clock::now();const auto gate=AutoGateLocked(now);
            const auto setStatus=[&](const std::string& value){if(r.autoStatus!=value){r.autoStatus=value;statusChanged=true;}};
            if(gate!="ready"){
                setStatus(gate);r.candidateTarget.clear();r.candidateSince={};
                if(gate=="waitingForLocation")r.autoDirty=true;
            }else{
                remaining=AutoRoute::Remaining(*r.active,r.completed);
                std::unordered_set<std::string> spacingKeys;for(const auto& item:remaining)spacingKeys.insert(AutoRoute::Key(item));
                if(spacingKeys!=r.spacingKeys){r.spacing=AutoRoute::TargetSpacing(remaining);r.spacingKeys=std::move(spacingKeys);}
                spacing=r.spacing;
                const auto& p=r.stablePlayer.last;
                const bool moved=!r.hasAutoPosition||std::hypot(p.roc.x-r.lastAutoPosition.x,p.roc.y-r.lastAutoPosition.y)>=spacing*.10;
                const bool due=r.lastAutoSolve==Clock::time_point{}||now-r.lastAutoSolve>=AutoRoute::ReplanPeriod;
                setStatus(ProximityValidLocked(now)&&r.proximity.distancePixels<AutoRoute::NearbyPixels?"nearTarget":
                    !r.candidateTarget.empty()?"confirmingTarget":r.lastTargetSwitch!=Clock::time_point{}&&
                    now-r.lastTargetSwitch<AutoRoute::TargetSwitchCooldown?"cooldown":"ready");
                if(remaining.size()>=2&&spacing>0&&due&&(r.autoDirty||moved||!r.candidateTarget.empty())){
                    player=p;original=*r.active;completed=r.completed;epoch=r.autoEpoch.load();orderRevision=r.orderRevision;
                    r.lastAutoSolve=now;r.lastAutoPosition=p.roc;r.hasAutoPosition=true;r.autoDirty=false;
                    r.autoComputing=true;setStatus("computing");calculate=true;
                }
            }
        }
        if(statusChanged)Emit();
        if(!calculate)continue;
        const auto began=Clock::now();
        try{
            AutoRoute::Start start{player.sceneId,player.roc,"autoPlayerFix",0,player.continuityGeneration,true};
            const auto result=AutoRoute::Solve(start,remaining,[&]{return r.autoEpoch.load()!=epoch;});
            std::string outcome="cancelled";double oldLength=0,newLength=0;
            {
                std::scoped_lock lock(r.mutex);const auto now=Clock::now();
                if(r.autoEpoch.load()!=epoch)continue;
                r.autoComputing=false;r.autoStatus="ready";
                const auto& latest=r.stablePlayer.last;
                const bool identity=AutoGateLocked(now)=="ready"&&r.active&&r.active->id==original.id&&r.profile==original.profileId&&
                    r.orderRevision==orderRevision&&r.completed==completed&&r.active->skipped==original.skipped&&
                    latest.sessionId==player.sessionId&&latest.continuityGeneration==player.continuityGeneration;
                // Completion storage is authoritative even before its asynchronous event reaches us.
                const bool completionUnchanged=std::all_of(original.stops.begin(),original.stops.end(),[&](const auto& item){
                    return CompletedNow(original.sceneId,item)==completed.contains(AutoRoute::Key(item));});
                if(result.cancelled||!identity||!completionUnchanged){r.autoDirty=true;r.candidateTarget.clear();r.candidateSince={};}
                else if(std::hypot(latest.roc.x-player.roc.x,latest.roc.y-player.roc.y)>spacing*.10){
                    r.autoDirty=true;r.candidateTarget.clear();r.candidateSince={};outcome="player-moved";
                }else if(!AutoRoute::Worthwhile(latest.roc,remaining,result.stops,spacing)){
                    r.candidateTarget.clear();r.candidateSince={};outcome="insufficient-gain";
                }else{
                    oldLength=AutoRoute::Length(latest.roc,remaining);newLength=AutoRoute::Length(latest.roc,result.stops);
                    const bool switches=AutoRoute::Key(remaining.front())!=AutoRoute::Key(result.stops.front());
                    bool apply=true;
                    if(switches){
                        const auto wait=AutoRoute::CheckTargetSwitch(AutoRoute::Key(result.stops.front()),ProximityValidLocked(now),
                            r.proximity.distancePixels,now,r.lastTargetSwitch,r.candidateTarget,r.candidateSince);
                        if(!wait.empty()){r.autoStatus=wait;apply=false;
                            if(wait=="waitingForLocation"||wait=="cooldown")r.autoDirty=true;}
                    }
                    if(apply){
                        auto next=AutoRoute::MergeRemaining(original,completed,result.stops);
                        next.start=start;next.start.roc=latest.roc;
                        next.start.confirmedUnixMs=std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()-(now-latest.fixCapturedAt)).count();
                        r.store->Save(next,false); // Save before publishing: failure keeps the old route usable.
                        if(switches){r.previousTarget=remaining.front();r.lastTargetSwitch=now;r.nearConfirmation.Reset();}
                        r.active=std::move(next);++r.orderRevision;++r.revision;r.proximity={};
                        r.candidateTarget.clear();r.candidateSince={};r.autoStatus="ready";
                        r.message=switches?"已根据当前位置调整目标；灰色虚线指向原目标":"已优化剩余访问顺序";
                        outcome="applied";
                    }else outcome=r.autoStatus;
                }
            }
            StructuredLogger::Record("info","routes","auto-route-replanned","outcome="+outcome+
                " targets="+std::to_string(remaining.size())+" oldLength="+std::to_string(oldLength)+" newLength="+std::to_string(newLength)+
                " elapsedMs="+std::to_string(std::chrono::duration<double,std::milli>(Clock::now()-began).count()));
            Emit();
        }catch(const std::exception& e){
            {std::scoped_lock lock(r.mutex);if(r.autoEpoch.load()!=epoch)continue;r.autoComputing=false;r.autoStatus="saveFailed";
                r.autoDirty=true;r.message=std::string("实时重排未生效，原路线保留：")+e.what();++r.revision;}Emit();
        }
    }
}
void BuildCatalogLocked(){
    auto& r=R();const std::array<const Json*,8> sources={&DrawItemBase::itemsJsonData_World,&DrawItemBase::itemsJsonData_Tethys,
        &DrawItemBase::itemsJsonData_Fabricatorium,&DrawItemBase::itemsJsonData_Avinoleum,&DrawItemBase::itemsJsonData_Lahai,
        &DrawItemBase::itemsJsonData_LowerVault,&DrawItemBase::itemsJsonData_Darkplain,&DrawItemBase::itemsJsonData_TimeRiftRuins};
    r.catalog.clear();r.names.clear();
    for(std::size_t i=0;i<sources.size();++i){const int scene=static_cast<int>(i+1);if(!sources[i]->is_array())continue;
        for(const auto& category:*sources[i]){
            const auto nameId=category.value("id","");r.names[nameId]=category.value("name",nameId);
            for(const auto& p:category.value("location",Json::array())){
                const auto coordinate=KuroPositionToGameCoordinates(p.at("x").get<double>(),p.at("y").get<double>());
                ItemDatas item{p.at("id").get<std::string>(),nameId,{},RelativeCoordinates::IdentifyCoordToROC(coordinate,scene),false};
                item.layer.stateId=p.value("stateId",Scene::Find(scene)->kuroStateId);if(item.layer.stateId<=0)item.layer.stateId=Scene::Find(scene)->kuroStateId;
                item.layer.countryId=p.value("countryId",0);
                const auto text=[&](const char* key){return !p.contains(key)||p.at(key).is_null()?std::string{}:p.at(key).is_string()?p.at(key).get<std::string>():p.at(key).dump();};
                item.layer.floorId=text("floorId");item.layer.level=text("level");r.catalog[scene][AutoRoute::Key(item)]=std::move(item);
            }
        }
    }
}
}

void RoutePlanningService::Initialize(){
    auto& r=R();{std::scoped_lock lock(r.mutex);if(r.ready)return;
        r.store=std::make_unique<AutoRoute::RoutePlanStore>(StructuredLogger::ApplicationDataDirectory()/"SavedRoutes"/"Auto");
        BuildCatalogLocked();r.stopping=false;r.ready=true;SyncProfileLocked();}
    r.worker=std::thread(Worker);
    r.autoWorker=std::thread(AutoWorker);
}
void RoutePlanningService::Shutdown(){
    auto& r=R();{std::scoped_lock lock(r.mutex);if(!r.ready)return;r.stopping=true;InvalidateLocked();r.wake.notify_all();r.autoWake.notify_all();}
    if(r.worker.joinable())r.worker.join();
    if(r.autoWorker.joinable())r.autoWorker.join();
    {std::scoped_lock lock(r.mutex);r.ready=false;r.enabled=false;r.runRequested=false;}SetEventCallback({});
}
void RoutePlanningService::SetEventCallback(std::function<void(const Json&)> callback){std::scoped_lock lock(R().eventMutex);R().callback=std::move(callback);}
Json RoutePlanningService::Snapshot(){std::scoped_lock lock(R().mutex);return SnapshotLocked();}
Json RoutePlanningService::GuideTarget(const Json& command){
    auto& r=R();
    try {
        std::scoped_lock lock(r.mutex);if(!r.ready)throw std::runtime_error("自动规划尚未初始化");
        SyncProfileLocked();
        if(command.value("profileId",std::string{})!=r.profile)throw std::runtime_error("档案已变化，请重新打开攻略");
        // Resolve against durable completion now, rather than an asynchronously
        // delivered UI snapshot or the target captured when F8 went down.
        RefreshCompletedLocked();
        Json selection=nullptr;const int target=TargetIndexLocked();
        if(target>=0){
            POINT cursor{};GetCursorPos(&cursor);
            const double x=command.value("screenX",static_cast<double>(cursor.x));
            const double y=command.value("screenY",static_cast<double>(cursor.y));
            if(!std::isfinite(x)||!std::isfinite(y))throw std::invalid_argument("攻略窗口坐标无效");
            const auto& item=r.active->stops[target];
            selection={{"type","markerSelected"},{"profileId",r.profile},{"sceneName",Scene::SceneIdToName(r.active->sceneId)},
                {"nameId",item.nameId},{"stateId",item.layer.stateId},{"pointId",item.itemId},
                {"countryId",item.layer.countryId},{"floorId",item.layer.floorId},{"level",item.layer.level},
                {"completed",false},{"screenX",x},{"screenY",y}};
        }
        return {{"accepted",true},{"data",{{"profileId",r.profile},{"routeId",r.active?r.active->id:std::string{}},
            {"revision",r.revision},{"selection",std::move(selection)}}}};
    }catch(const std::exception& e){return {{"accepted",false},{"message",e.what()},{"data",Json::object()}};}
}
RoutePlanningView RoutePlanningService::View(){
    auto& r=R();std::scoped_lock lock(r.mutex);RoutePlanningView v;
    v.enabled=r.enabled;v.computing=r.computing;v.profileId=r.profile;v.tool=r.tool;v.message=r.message;v.sceneId=r.scene;
    v.revision=r.revision;v.generation=r.epoch.load();v.hiddenCount=HiddenLocked();v.active=r.active;v.completed=r.completed;
    v.mapStart=r.mapStart;
    v.autoReplanEnabled=r.autoEnabled;v.autoReplanComputing=r.autoComputing;v.autoReplanStatus=r.autoStatus;
    v.orderRevision=r.orderRevision;v.previousTarget=r.previousTarget;
    v.currentTargetIndex=TargetIndexLocked();v.navigationStatus=NavigationLocked();v.navigating=v.navigationStatus=="navigating";
    const auto it=r.drafts.find(r.scene);if(it!=r.drafts.end()){v.selected=it->second.selected;v.start=it->second.start;v.preview=it->second.preview;}return v;
}
AutoRoute::DrawVisibility RoutePlanningService::DrawingVisibility(){
    auto& r=R();std::scoped_lock lock(r.mutex);
    AutoRoute::DrawVisibility result;result.profileId=r.profile;
    if(r.active)result.activeId=r.active->id;
    result.orderRevision=r.orderRevision;
    result.comparisonVisible=r.autoEnabled&&r.runRequested&&r.previousTarget.has_value()&&
        (r.observedScene? r.mapStart.valid&&r.active&&r.mapStart.sceneId==r.active->sceneId : r.stablePlayer.last.Fresh(Clock::now()));
    const auto draft=r.drafts.find(r.scene);
    if(r.enabled&&draft!=r.drafts.end()&&draft->second.preview)result.previewId=draft->second.preview->id;
    result.navigating=NavigationLocked()=="navigating";return result;
}
bool RoutePlanningService::PlanningMode(){std::scoped_lock lock(R().mutex);return R().enabled;}
void RoutePlanningService::ObserveMap(int scene,const std::vector<ItemDatas>& visible){
    auto& r=R();bool changed=false;
    {std::scoped_lock lock(r.mutex);if(!r.ready||!Scene::IsKnown(scene))return;
        const auto before=HiddenLocked();
        if(r.observedScene!=scene){r.observedScene=scene;if(r.scene!=scene){InvalidateLocked();r.scene=scene;r.tool="pan";changed=true;}}
        std::unordered_set<std::string> keys;for(const auto& p:visible)keys.insert(AutoRoute::Key(p));
        if(keys!=r.visibleKeys){++r.revision;r.visibilityEventPending=true;}
        r.visible=visible;r.visibleKeys=std::move(keys);
        if(r.enabled){auto& draft=r.drafts[scene];if(r.pendingNew){draft={};r.pendingNew=false;++r.revision;changed=true;}
            if(!draft.start.valid&&r.mapStart.valid&&r.mapStart.sceneId==scene){draft.start=r.mapStart;++r.revision;changed=true;}}
        if(HiddenLocked()!=before)r.visibilityEventPending=true;
        if(r.visibilityEventPending&&Clock::now()-r.lastVisibilityEvent>=std::chrono::milliseconds(200)){
            r.visibilityEventPending=false;r.lastVisibilityEvent=Clock::now();changed=true;
        }
        // Sending a throttled visibility event does not itself change the
        // selection context or invalidate a gesture begun after the last pan.
    }if(changed)Emit();
}
void RoutePlanningService::SessionStopped(){
    auto& r=R();{std::scoped_lock lock(r.mutex);if(!r.ready)return;
        InvalidateAutoLocked(true);r.stablePlayer.Reset();r.hasAutoPosition=false;
        InvalidateLocked();r.playerAvailable=false;r.player={};r.mapStart={};r.observedScene=0;r.visible.clear();r.visibleKeys.clear();
        for(auto& [scene,draft]:r.drafts)if(draft.start.source!="manual"&&!draft.preview)draft.start={};
        r.message="游戏定位已停止，路线保留";}Emit();
}
void RoutePlanningService::MapUnavailable(){
    auto& r=R();bool changed=false;{std::scoped_lock lock(r.mutex);if(r.observedScene){r.observedScene=0;r.visible.clear();r.visibleKeys.clear();
        if(r.computing)InvalidateLocked();++r.revision;changed=true;}}if(changed)Emit();
}
void RoutePlanningService::CaptureMapStart(const AutoRoute::Start& start){
    auto& r=R();{std::scoped_lock lock(r.mutex);r.mapStart=start;r.playerAvailable=false;
        r.stablePlayer.Reset();r.proximity={};r.nearConfirmation.Reset();
        InvalidateLocked();
        // A generated preview has an explicit fixed start. Only unsolved drafts
        // adopt the new map-entry position; the active route is never rewritten.
        for(auto& [scene,draft]:r.drafts)if(draft.start.source!="manual"&&!draft.preview)
            draft.start=start.valid&&start.sceneId==scene?start:AutoRoute::Start{};
        ++r.revision;}Emit();
}
void RoutePlanningService::UpdatePlayer(const AutoRoute::Start& position){
    auto& r=R();bool changed;{std::scoped_lock lock(r.mutex);const auto before=NavigationLocked();r.player=position;r.playerAvailable=position.valid;
        changed=before!=NavigationLocked();if(changed)++r.revision;}if(changed)Emit();
}
void RoutePlanningService::SetPlayerAvailable(bool available){
    auto& r=R();bool changed;{std::scoped_lock lock(r.mutex);const auto before=NavigationLocked();r.playerAvailable=available&&r.player.valid;
        changed=before!=NavigationLocked();if(changed)++r.revision;}if(changed)Emit();
}
void RoutePlanningService::SetAutoReplanEnabled(bool enabled){
    auto& r=R();bool changed=false;{std::scoped_lock lock(r.mutex);
        if(r.autoEnabled!=enabled){r.autoEnabled=enabled;InvalidateAutoLocked(!enabled);r.hasAutoPosition=false;
            r.autoStatus=enabled?"waitingForLocation":"disabled";++r.revision;changed=true;}}
    if(changed)Emit();
}
void RoutePlanningService::ObservePlayer(const AutoRoute::PlayerObservation& observation){
    auto& r=R();std::scoped_lock lock(r.mutex);if(!r.ready)return;
    auto next=observation;if(next.profileId!=r.profile)next.valid=false;
    const auto prior=r.stablePlayer.last;const bool had=r.stablePlayer.count>0;
    r.stablePlayer.Observe(next,Clock::now());
    if(had&&(!r.stablePlayer.count||prior.sessionId!=next.sessionId||prior.sceneId!=next.sceneId||
        prior.continuityGeneration!=next.continuityGeneration)){
        InvalidateAutoLocked();r.proximity={};r.nearConfirmation.Reset();
    }
}
void RoutePlanningService::ObserveProximity(const AutoRoute::ProximityObservation& observation){
    auto& r=R();bool changed=false;{std::scoped_lock lock(r.mutex);if(!r.ready)return;
        // An observation for a superseded target cannot erase or replace its successor's evidence.
        const int target=TargetIndexLocked();
        if(!r.active||target<0||observation.profileId!=r.profile||observation.routeId!=r.active->id||
            observation.orderRevision!=r.orderRevision||observation.targetKey!=AutoRoute::Key(r.active->stops[target]))return;
        r.proximity=observation;const auto now=Clock::now();
        r.proximity.valid=r.proximity.valid&&r.stablePlayer.last.Fresh(now)&&ProximityValidLocked(now);
        if(r.nearConfirmation.Observe(r.proximity,now)&&r.previousTarget){
            r.previousTarget.reset();++r.orderRevision;++r.revision;r.proximity={};r.nearConfirmation.Reset();changed=true;
        }
    }if(changed)Emit();
}
void RoutePlanningService::OnMarkerChanged(){
    auto& r=R();{std::scoped_lock lock(r.mutex);if(!r.ready)return;const auto old=TargetIndexLocked();
        const auto priorCompleted=r.completed;SyncProfileLocked();RefreshCompletedLocked();
        const bool activeProgressChanged=r.active&&std::any_of(r.active->stops.begin(),r.active->stops.end(),[&](const auto& p){
            const auto key=AutoRoute::Key(p);return priorCompleted.contains(key)!=r.completed.contains(key);});
        if(activeProgressChanged)InvalidateAutoLocked(true);
        if(r.computing){InvalidateLocked();r.message="目标完成状态已变化，请重新生成路线";}
        const auto next=TargetIndexLocked();if(next>=0&&(old<0||next<old))r.message="已恢复前面的目标，按原顺序继续";
        ++r.revision;}Emit();
}
Json RoutePlanningService::AddPoints(const std::vector<ItemDatas>& points,const Json& context){
    std::vector<std::string> keys;for(const auto& point:points)keys.push_back(AutoRoute::Key(point));auto command=context;
    command["action"]="add";command["keys"]=keys;return Command(command);
}
Json RoutePlanningService::TogglePoint(const ItemDatas& point,const Json& context){auto command=context;command["action"]="toggle";command["key"]=AutoRoute::Key(point);return Command(command);}
Json RoutePlanningService::SetManualStart(int scene,const Coordinate& roc,const Json& context){auto command=context;
    command["action"]="setStart";command["sceneId"]=scene;command["x"]=roc.x;command["y"]=roc.y;return Command(command);}
Json RoutePlanningService::Command(const Json& command){
    auto& r=R();Json result;
    try {
        std::unique_lock lock(r.mutex);if(!r.ready)throw std::runtime_error("自动规划尚未初始化");SyncProfileLocked();
        if(command.contains("profileId")&&command.at("profileId")!=r.profile)throw std::runtime_error("档案已变化，请刷新路线");
        if(command.contains("expectedRevision")&&command.at("expectedRevision")!=r.revision)throw std::runtime_error("路线状态已变化，请刷新后重试");
        if(command.contains("expectedSceneId")&&command.at("expectedSceneId")!=r.scene)throw std::runtime_error("选点地图已变化，本次操作未生效");
        if(command.contains("expectedGeneration")&&command.at("expectedGeneration")!=r.epoch.load())throw std::runtime_error("选点草稿已变化，本次操作未生效");
        const auto action=command.value("action","state");
        if(command.contains("routeId")&&action!="load"&&action!="delete"&&(!r.active||command.at("routeId")!=r.active->id))
            throw std::runtime_error("活动路线已变化，请刷新后重试");
        if((action=="complete"||action=="skip"||action=="guide")&&command.contains("key")){
            const auto target=TargetIndexLocked();if(target<0||command.at("key")!=AutoRoute::Key(r.active->stops[target]))
                throw std::runtime_error("当前目标已变化，本次操作未生效");
        }
        if(action=="new"){
            InvalidateLocked();r.enabled=true;r.tool="pan";r.scene=command.value("sceneId",r.observedScene);
            if(Scene::IsKnown(r.scene)){r.drafts[r.scene]={};if(r.mapStart.valid&&r.mapStart.sceneId==r.scene)r.drafts[r.scene].start=r.mapStart;r.pendingNew=false;}
            else {r.scene=0;r.pendingNew=true;}r.message="点击点位切换选中，或用框选、套索批量添加目标";
        }else if(action=="end"){InvalidateLocked();r.enabled=false;r.tool="pan";r.message="已退出选点，草稿保留";}
        else if(action=="tool"){
            const auto tool=command.value("tool","pan");if(tool!="pan"&&tool!="box"&&tool!="rectangle"&&tool!="lasso"&&tool!="point"&&tool!="start")throw std::invalid_argument("选点工具无效");
            r.tool=tool=="rectangle"?"box":tool;r.enabled=true;
        }else if(action=="setStart"){
            if(command.value("sceneId",0)!=r.scene)throw std::runtime_error("起点不属于当前地图");
            const Coordinate roc(command.at("x").get<double>(),command.at("y").get<double>());if(!Finite(roc))throw std::runtime_error("起点坐标无效");
            auto& draft=DraftLocked();InvalidateLocked();draft.start={r.scene,roc,"manual",0,r.epoch.load(),true};draft.preview.reset();r.tool="pan";r.message="已设置手动起点";
        }else if(action=="add"||action=="addVisible"){
            std::vector<std::string> keys;
            if(action=="add") keys=command.at("keys").get<std::vector<std::string>>();
            else for(const auto& p:r.visible)if(AutoRoute::IsSurfaceTarget(p))keys.push_back(AutoRoute::Key(p));
            AddLocked(keys);
        }else if(action=="toggle"||action=="remove"){
            auto& draft=DraftLocked();const auto key=command.at("key").get<std::string>();
            const auto found=std::find_if(draft.selected.begin(),draft.selected.end(),[&](const ItemDatas& p){return AutoRoute::Key(p)==key;});
            if(found!=draft.selected.end()){const auto index=std::distance(draft.selected.begin(),found);RememberSelectionLocked(draft);draft.selected.erase(draft.selected.begin()+index);}
            else if(action=="toggle")AddLocked({key});
        }else if(action=="undo"){
            auto& draft=DraftLocked();if(!draft.history.empty()){draft.selected=std::move(draft.history.back());draft.history.pop_back();draft.preview.reset();InvalidateLocked();RefreshCompletedLocked();}
        }else if(action=="clear"){
            auto& draft=DraftLocked();if(!draft.selected.empty()){RememberSelectionLocked(draft);draft.selected.clear();}
        }else if(action=="generate"||action=="replan"){
            if(action=="replan"&&!r.observedScene&&r.active)r.scene=r.active->sceneId;QueueSolveLocked(action=="replan");
        }else if(action=="activate"){
            auto& draft=DraftLocked();if(!draft.preview)throw std::runtime_error("请先生成路线预览");
            InvalidateLocked();
            RefreshCompletedLocked();r.store->Save(*draft.preview,true);r.active=*draft.preview;r.skipHistory.clear();r.runRequested=true;r.enabled=false;r.tool="pan";
            InvalidateAutoLocked(true);r.hasAutoPosition=false;
            draft.preview.reset();
            r.saved=r.store->List(r.profile);r.message="自动路线已保存并开始导航";
        }else if(action=="stop"){
            // Persist the exit before changing runtime state. A failed write
            // leaves the previous route usable and reports the failure.
            r.store->ClearActive(r.profile);StopNavigationLocked();
            RefreshCompletedLocked();r.message="已退出导航并隐藏路线，保存的路线和完成记录保留";
        }else if(action=="delete"){
            const auto id=command.at("routeId").get<std::string>();
            const bool removingActive=r.active&&AutoRoute::SameRouteId(r.active->id,id);
            try {r.store->Delete(r.profile,id);}
            catch(const AutoRoute::DeleteRollbackFailure&){
                if(removingActive)StopNavigationLocked();
                throw;
            }
            if(removingActive)StopNavigationLocked();
            else for(auto& [scene,draft]:r.drafts)if(draft.preview&&AutoRoute::SameRouteId(draft.preview->id,id)){draft.preview.reset();InvalidateLocked();}
            RefreshCompletedLocked();r.saved=r.store->List(r.profile);
            r.message=removingActive?"路线已删除并退出导航，点位完成记录保留":"路线已删除，点位完成记录保留";
        }else if(action=="pause"){r.runRequested=false;InvalidateAutoLocked();r.message="导航已暂停；如需隐藏并结束路线，请退出导航";}
        else if(action=="resume"){
            if(!r.active)throw std::runtime_error("请先加载或生成路线");InvalidateLocked();r.runRequested=true;r.enabled=false;r.tool="pan";r.message="已继续导航";
        }else if(action=="skip"||action=="undoSkip"){
            if(!r.active)throw std::runtime_error("当前没有活动路线");auto next=*r.active;std::string key;
            if(action=="skip"){const int target=TargetIndexLocked();if(target<0)throw std::runtime_error("路线已结束");key=AutoRoute::Key(next.stops[target]);next.skipped.insert(key);}
            else {if(r.skipHistory.empty())throw std::runtime_error("没有可以撤销的跳过操作");key=r.skipHistory.back();next.skipped.erase(key);}
            next.skipHistory=r.skipHistory;if(action=="skip")next.skipHistory.push_back(key);else next.skipHistory.pop_back();
            r.store->Save(next,false);r.active=std::move(next);
            InvalidateAutoLocked(true);
            if(action=="skip")r.skipHistory.push_back(key);else r.skipHistory.pop_back();
            r.message=action=="skip"?"已跳过当前目标，完成记录未修改":"已撤销跳过，按原顺序继续";
        }else if(action=="guide"){
            const int target=TargetIndexLocked();if(target<0||!r.active)throw std::runtime_error("当前没有可查看攻略的导航目标");
            const auto item=r.active->stops[target];const auto profile=r.profile;const auto routeId=r.active->id;
            const auto scene=Scene::SceneIdToName(r.active->sceneId);POINT cursor{};GetCursorPos(&cursor);
            // This publishes the existing markerSelected event. It never
            // changes completion or advances the route.
            lock.unlock();DrawItemBase::SelectMarker(scene,item,cursor,profile);lock.lock();
            if(r.profile==profile&&r.active&&r.active->id==routeId)r.message="已打开当前目标攻略";
        }else if(action=="complete"){
            const int target=TargetIndexLocked();if(target<0||!r.active)throw std::runtime_error("当前没有待完成目标");
            const auto item=r.active->stops[target];const auto profile=r.profile;const auto routeId=r.active->id;
            lock.unlock();const auto completed=DrawItemBase::HandleMarkerCommand({{"type","markerSetCompletion"},{"profileId",profile},
                {"stateId",item.layer.stateId},{"pointId",item.itemId},{"completed",true}});
            if(!completed.value("accepted",false))throw std::runtime_error(completed.value("message","完成状态保存失败"));
            lock.lock();if(r.profile==profile&&r.active&&r.active->id==routeId)r.message="完成状态已保存";
        }else if(action=="save"){
            AutoRoute::Plan plan;const bool preview=command.value("target","")=="preview"||!r.active;
            if(preview){const auto& draft=DraftLocked();if(!draft.preview)throw std::runtime_error("没有可保存的预览");plan=*draft.preview;
                if(r.active&&r.active->id==plan.id)throw std::runtime_error("该预览已成为活动路线，请保存活动路线");}
            else plan=*r.active;
            plan.name=command.value("name",plan.name);r.store->Save(plan,!preview);
            if(preview)r.drafts[r.scene].preview=plan;else r.active=plan;r.saved=r.store->List(r.profile);r.message="自动路线已保存";
        }else if(action=="load"){
            const auto next=r.store->Load(r.profile,command.at("routeId").get<std::string>(),ResolveLocked);
            r.store->Save(next,true);InvalidateLocked();r.active=next;r.runRequested=false;r.skipHistory=next.skipHistory;r.enabled=false;r.tool="pan";
            InvalidateAutoLocked(true);r.hasAutoPosition=false;
            for(auto& [scene,draft]:r.drafts)if(draft.preview&&draft.preview->id==next.id)draft.preview.reset();
            RefreshCompletedLocked();r.message="路线已加载，点击继续导航";
        }else if(action=="list"){r.saved=r.store->List(r.profile);}
        else if(action!="state")throw std::invalid_argument("未知的自动路线操作");
        ++r.revision;result={{"accepted",true},{"message",r.message},{"data",SnapshotLocked()}};
    }catch(const std::exception& e){std::scoped_lock lock(r.mutex);r.message=e.what();++r.revision;result={{"accepted",false},{"message",r.message},{"data",SnapshotLocked()}};}
    Emit();return result;
}
