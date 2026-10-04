#include "RoutePlanningService.h"
#include "RoutePlanStore.h"
#include "RouteCollections.h"
#include "RouteBundle.h"
#include "LegacyHandRouteImportFile.h"
#include "HandDrawnRoute.h"
#include "FarmMode.h"
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
#include <limits>
#include <map>
#include <mutex>
#include <set>
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
    // A replan of the active route is still the same route, so the preview it produces has to
    // carry that route's farming setting. Without this, 重新规划 followed by 开始指引 would
    // quietly switch the mode off while the player is standing on a farming route.
    bool farmMode=false;
    // Where the resulting preview belongs. Empty means "not decided yet": a freshly planned route
    // belongs to whichever collection the player is in when they *save* it, not to the one they
    // happened to be in when they pressed 生成预览 — otherwise saving a preview after switching
    // collections would file it in the wrong place. A replan is the exception, and says so by
    // filling this in: it is still the route it came from, and re-applying it must not move that
    // route into whichever collection the player is looking at.
    std::string collection;
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
    bool mapSuspended=false;
    std::string profile,tool="pan",message;
    bool deletionRecoveryPending=false;
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
    std::unique_ptr<AutoRoute::RouteCollections> collectionStore;
    Json saved=Json::array();
    // Which collections exist for this profile and which one the player is in. The current one is
    // where a newly saved route lands; a route that is already saved keeps its own, because moving
    // the player's existing work just because they looked at another collection is not a thing they
    // asked for.
    AutoRoute::RouteCollections::Index collections;
    std::string currentCollection=AutoRoute::DefaultCollectionId;
    // What the last `importInspect` found, shown to the player before anything moves. It is the
    // reason the import can be two steps: the file's kind, the names and the conflict are answered
    // here, so the interface never has to parse a route package itself.
    Json transfer=nullptr;
    std::function<void(const Json&)> callback;
    Clock::time_point lastVisibilityEvent{};
    bool visibilityEventPending=false;
    bool autoEnabled=false, autoComputing=false, autoDirty=true;
    std::string autoStatus="disabled", candidateTarget;
    // Farming mode is a property of the navigation session, not a saved setting: it exists
    // only while an active route is being followed, and stopping the navigation ends it.
    bool farmMode=false;
    std::string farmNotice;
    std::uint64_t farmNoticeSerial=0;
    FarmMode::Confirmation farmConfirmation;
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
    // The route being drawn by hand. It lives here rather than in a class of its own so the
    // drawing is cancelled and reported by exactly the same events as everything else.
    AutoRoute::HandDrawnDraft handDraft;
    bool handTypeChoosing=false;
};
Runtime& R(){static Runtime state;return state;}
// The identity a drawing in progress borrows while it is still being drawn. It never reaches
// disk: it exists so the renderer can treat the unfinished line as an ordinary preview.
constexpr char HandDraftId[]="hand-draft";
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
    // Farming mode belongs to the navigation it was switched on for. Leaving the navigation
    // turns it off rather than letting it wait for a route the player has put away.
    r.farmMode=false;r.farmConfirmation.Reset();r.farmNotice.clear();
    r.enabled=false;r.pendingNew=false;r.tool="pan";
    for(auto& [scene,draft]:r.drafts)draft.preview.reset();
}
Draft& DraftLocked(){auto& r=R();if(!Scene::IsKnown(r.scene))throw std::runtime_error("请先打开大地图并完成识别");return r.drafts[r.scene];}
std::optional<ItemDatas> ResolveLocked(int scene,const std::string& key){
    const auto sceneIt=R().catalog.find(scene);if(sceneIt==R().catalog.end())return {};
    const auto it=sceneIt->second.find(key);return it==sceneIt->second.end()?std::optional<ItemDatas>{}:it->second;
}
bool CompletedNow(int scene,const ItemDatas& item){return DrawItemBase::IsPointCompleted(Scene::SceneIdToName(scene),item);}
// A point type as the interface needs to show it: the name the game uses and the icon the map
// draws. The icon manifest knows far more types than the filter catalogue does, so resolving both
// here is what lets a route row show "叮咚咚 + its icon" for a type that has no filter row at all.
Json KindJsonLocked(const std::string& nameId){
    const auto& names=R().names;const auto found=names.find(nameId);
    const auto name=found==names.end()||found->second.empty()?nameId:found->second;
    const auto icon=DrawItemBase::GetExternalIconPath(nameId);
    return {{"nameId",nameId},{"name",name},{"icon",icon}};
}
// The saved-route rows the list shows. Each row describes what it is made of, and the type
// descriptors are resolved here rather than left for the interface to look up.
// The rows the interface sees, with every collection the index does not know mapped onto the
// default one. A route file can name a collection that no longer exists — the index was deleted,
// the file was edited by hand, the profile was copied around — and such a route must stay visible
// and usable rather than falling out of every list at once.
void ReloadSavedLocked(){
    auto& r=R();
    r.saved=r.store->List(r.profile);
    for(auto& row:r.saved){
        const auto named=AutoRoute::NormalizeCollectionId(row.value("collection",std::string{AutoRoute::DefaultCollectionId}));
        row["collection"]=AutoRoute::RouteCollections::Exists(r.collections,named)?named:std::string(AutoRoute::DefaultCollectionId);
    }
}
// How many routes each collection holds, counted from the rows the list is about to show rather
// than asked of the store a second time: the count and the rows can then never disagree.
Json CollectionsJsonLocked(){
    const auto& r=R();
    const auto count=[&](const std::string& id){
        std::size_t total=0;
        for(const auto& row:r.saved)
            if(AutoRoute::SameRouteId(row.value("collection",std::string{AutoRoute::DefaultCollectionId}),id))++total;
        return total;
    };
    Json rows=Json::array();
    // The default collection is always first and always present, and it is not stored anywhere:
    // it is the bucket every route already had before collections existed.
    rows.push_back({{"id",AutoRoute::DefaultCollectionId},{"name",AutoRoute::DefaultCollectionName},
        {"routeCount",count(AutoRoute::DefaultCollectionId)},{"current",AutoRoute::IsDefaultCollection(r.currentCollection)},
        {"system",true}});
    for(const auto& collection:r.collections.collections)
        rows.push_back({{"id",collection.id},{"name",collection.name},{"routeCount",count(collection.id)},
            {"current",AutoRoute::SameRouteId(collection.id,r.currentCollection)},{"system",false}});
    return rows;
}
// Re-read the index and make the pointer agree with the rows. Nothing else in the service touches
// the collections file, so this is the one place a collection can appear or disappear from the
// snapshot.
void ReloadCollectionsLocked(){
    auto& r=R();
    r.collections=r.collectionStore->Load(r.profile);
    r.currentCollection=AutoRoute::NormalizeCollectionId(r.collections.current);
    if(!AutoRoute::RouteCollections::Exists(r.collections,r.currentCollection))
        r.currentCollection=AutoRoute::DefaultCollectionId;
}
// Writing the index is always the second half of changing it: the pointer moves only once the file
// says so, so a failed write leaves the collections as they were instead of promising a switch
// that was never recorded.
void SaveCollectionsLocked(){
    auto& r=R();
    r.collections.current=r.currentCollection;
    r.collectionStore->Save(r.profile,r.collections);
}
// A mutable handle on a stored collection, or null. The index's own Find is const so that a caller
// which only wants to read cannot change the file by accident.
AutoRoute::Collection* MutableCollectionLocked(const std::string& id){
    auto& r=R();
    const auto normalized=AutoRoute::NormalizeCollectionId(id);
    for(auto& collection:r.collections.collections)
        if(AutoRoute::SameRouteId(collection.id,normalized))return &collection;
    return nullptr;
}
std::string CollectionNameLocked(const std::string& id){
    const auto* found=AutoRoute::RouteCollections::Find(R().collections,id);
    return found?found->name:std::string(AutoRoute::DefaultCollectionName);
}
// Everything that has to happen once a route file is gone, in one place: the navigation it was
// running and the preview that was standing in for it. Deleting one route and deleting a whole
// collection must not drift apart on this.
void DeleteRouteLocked(const std::string& routeId) {
    auto& r=R();
    auto canonical=routeId;
    for(const auto& row:r.saved)if(AutoRoute::SameRouteId(row.value("id",std::string{}),routeId)) {canonical=row.at("id").get<std::string>();break;}
    r.deletionRecoveryPending=true;
    r.store->Delete(r.profile,canonical,[&]{DrawItemBase::RemoveFreePointCompletions(r.profile,canonical);});
}
void ForgetDeletedRouteLocked(const std::string& routeId){
    auto& r=R();
    if(r.active&&AutoRoute::SameRouteId(r.active->id,routeId))StopNavigationLocked();
    else for(auto& [scene,draft]:r.drafts)
        if(draft.preview&&AutoRoute::SameRouteId(draft.preview->id,routeId)){draft.preview.reset();InvalidateLocked();}
}
std::int64_t NowUnixMs(){
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
// Which collection a route belongs to at the moment it is first written. An empty value means the
// plan never made that decision — a freshly planned preview does not know where it will be filed
// until the player saves it, and a drawing never knows at all — so those take the current
// collection. A plan that already names one (a replan, or a re-save of a preview after the
// collection was deleted) keeps it when it still exists.
std::string NewRouteCollectionLocked(const AutoRoute::Plan& plan){
    const auto& r=R();
    if(plan.collection.empty())return r.currentCollection;
    const auto named=AutoRoute::NormalizeCollectionId(plan.collection);
    return AutoRoute::RouteCollections::Exists(r.collections,named)?named:r.currentCollection;
}
// Route ids and names are compared the way the store compares them — case-insensitively — so the
// two places that answer "is this taken?" cannot disagree with the rest of the code.
std::string LowerKey(const std::string& value){
    std::string result=value;
    std::transform(result.begin(),result.end(),result.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    return result;
}
std::size_t CollectionRouteCountLocked(const std::string& collectionId){
    std::size_t total=0;
    for(const auto& row:R().saved)
        if(AutoRoute::SameRouteId(row.value("collection",std::string{AutoRoute::DefaultCollectionId}),collectionId))++total;
    return total;
}
// The documents in a bundle, turned into plans this profile could store. It never writes anything:
// the caller decides where the point of no return is, which is what lets an import be inspected and
// let go of without one byte changing. A route that cannot be turned into a plan is counted and
// skipped rather than sinking the whole bundle, the way the legacy route import already behaves.
void ValidateTypedRoute(const AutoRoute::Plan& plan) {
    if(!plan.handDrawn || plan.legacyHandDrawn)return;
    for(const auto& item:plan.stops)if(!AutoRoute::IsFreeStop(item)) {
        const bool allowed=plan.routeCategory==FreePointCategory::Collectible ?
            DrawItemBase::IsCollectiblePoint(item.nameId) : DrawItemBase::IsRefreshablePoint(item.nameId);
        if(!allowed)throw std::invalid_argument("官方点位分类与手绘路线类型不匹配");
    }
}
std::vector<AutoRoute::Plan> ParseBundledRoutesLocked(const AutoRoute::RouteBundle::Contents& bundle,std::size_t& skipped){
    const auto& r=R();std::vector<AutoRoute::Plan> plans;
    for(const auto& document:bundle.routes){
        try{
            auto entry=document;
            entry["profileId"]=r.profile;
            // The id is the importer's business — it has to be free in the target profile — so all
            // this needs is a shape the document can be judged with. What is being judged is the
            // scene, the points and whether their resources still match.
            if(!entry.contains("id")||!entry.at("id").is_string()||!AutoRoute::IsRouteComponent(entry.at("id").get<std::string>()))
                entry["id"]=std::string("imported");
            auto plan=AutoRoute::RoutePlanStore::Parse(entry,r.profile,entry.at("id").get<std::string>(),ResolveLocked);
            ValidateTypedRoute(plan);plans.push_back(std::move(plan));
        }catch(const std::exception& error){
            ++skipped;
            StructuredLogger::Record("warn","routes","route-bundle-route-skipped",std::string("reason=")+error.what());
        }
    }
    return plans;
}
// A name that is free in the collection the route is about to join. Two routes with the same name in
// one collection are indistinguishable in a list, so the second one is numbered — the same rule the
// collections themselves follow.
std::string UniqueRouteNameLocked(const std::string& wanted,const std::set<std::string>& taken){
    if(!taken.contains(LowerKey(wanted)))return wanted;
    for(std::size_t nth=2;nth<=9999;++nth){
        const auto suffix=" ("+std::to_string(nth)+")";
        // The store caps a name at 256 characters; numbering must not push a long name over it.
        const auto stem=wanted.size()+suffix.size()<=256?wanted:wanted.substr(0,256-suffix.size());
        const auto candidate=stem+suffix;
        if(!taken.contains(LowerKey(candidate)))return candidate;
    }
    throw std::runtime_error("无法为导入的路线取一个可用的名字");
}
// The list the player sees grouped by collection, so a row and its collection come from one place.
Json SavedRoutesJsonLocked(){
    const auto& r=R();Json rows=Json::array();
    for(const auto& row:r.saved){
        Json kinds=Json::array();
        if(row.contains("kinds")&&row.at("kinds").is_array())
            for(const auto& kind:row.at("kinds"))
                kinds.push_back(KindJsonLocked(kind.value("nameId",std::string{})));
        Json next=row;next["kinds"]=std::move(kinds);rows.push_back(std::move(next));
    }
    return rows;
}
// Drawing a route by hand, one press per point. The point is recorded from the cursor, so "did the
// player mean an existing point or an empty spot?" is answered here, in ROC space, with a radius
// small enough that it only catches a deliberate hit: 1 ROC unit is about 1.2 map pixels, and the
// drawn marker radius is around 10 pixels, so this is roughly a third of the icon.
constexpr double HandDrawSnapRoc=3.0;
std::optional<ItemDatas> NearestCatalogPointLocked(int scene,const Coordinate& position,double tolerance){
    const auto sceneIt=R().catalog.find(scene);
    if(sceneIt==R().catalog.end())return {};
    const ItemDatas* best=nullptr;double bestDistance=0;
    for(const auto& [key,item]:sceneIt->second){
        const double distance=std::hypot(item.itemMapROC.x-position.x,item.itemMapROC.y-position.y);
        if(!best||distance<bestDistance){best=&item;bestDistance=distance;}
    }
    if(!best||bestDistance>tolerance)return {};
    return *best;
}
// Drawing a route by hand, one press per point. Every action is refused with a message rather than
// silently doing nothing, because the player is pressing a key on the game and has no other
// feedback channel.
void HandleHandDrawnLocked(const std::string& action,const Json& command){
    auto& r=R();
    if(!Scene::IsKnown(r.scene)||!r.observedScene)throw std::runtime_error("请先在游戏大地图上打开要绘制的区域");
    const auto* scene=Scene::Find(r.scene);
    if(!scene||scene->kuroStateId<=0)throw std::runtime_error("当前地图没有点位数据，无法手绘");
    if(r.handDraft.Active()&&r.handDraft.SceneId()!=r.scene)
        throw std::invalid_argument("请返回正在手绘的地图，或先完成、放弃当前路线");
    if(action=="handChoose"){InvalidateLocked();r.enabled=false;r.tool="pan";r.handTypeChoosing=true;r.message="请选择手绘路线类型";return;}
    if(action=="handIcon"){r.handDraft.SelectIcon(AutoRoute::ParseIcon(command.at("icon").get<std::string>()));return;}
    if(action=="handStart"){
        // Resuming is deliberate: with a finished drawing still unsaved, starting again continues it
        // rather than silently discarding the points the player came back for.
        const bool resuming=r.handDraft.Pending();
        if(r.handDraft.Active() && r.handDraft.CategoryLocked()) {
            if(command.contains("category")&&AutoRoute::ParseCategory(command.at("category").get<std::string>())!=r.handDraft.Category())
                throw std::invalid_argument("已经开始加点，路线类型不能更改");
            return;
        }
        if(!resuming&&!command.contains("category")){InvalidateLocked();r.enabled=false;r.tool="pan";r.handTypeChoosing=true;r.message="请选择收集物或非收集物路线";return;}
        const auto category=resuming?r.handDraft.Category():AutoRoute::ParseCategory(command.at("category").get<std::string>());
        InvalidateLocked();r.enabled=false;r.tool="pan";
        r.handDraft.Start(r.scene,scene->kuroStateId,category,resuming?r.handDraft.RouteId():NewId());
        r.handTypeChoosing=false;
        r.message=resuming ? "已继续手绘：接着画下去，画完按 Esc 结束"
                           : "手绘已开始：点击大地图上的位置记下一个点，按 Esc 结束手绘（已画的点会保留）";
        StructuredLogger::Record("info","routes","hand-drawn-start",
            "scene="+std::to_string(r.scene)+" resuming="+std::to_string(resuming?1:0)+
            " points="+std::to_string(r.handDraft.Size()));
        return;
    }
    if(action=="handFinish"){
        if(r.handTypeChoosing){r.handTypeChoosing=false;r.message="已取消类型选择";return;}
        // Leaving the drawing keeps it. The player cannot reach the toolbar while the drawing still
        // owns the map, so throwing the points away here would destroy work they never got to save.
        if(!r.handDraft.Active())throw std::runtime_error("当前没有正在绘制的手绘路线");
        const auto kept=r.handDraft.Finish();
        r.message=kept ? "已结束手绘，画好的 "+std::to_string(r.handDraft.Size())+" 个点已保留，可在路线列表里保存或放弃"
                       : "已结束手绘（还没有记下任何点）";
        StructuredLogger::Record("info","routes","hand-drawn-finished",
            "kept="+std::to_string(kept?1:0)+" points="+std::to_string(r.handDraft.Size()));
        return;
    }
    if(action=="handDiscard"){
        if(!r.handDraft.Pending()&&!r.handDraft.Active())throw std::runtime_error("当前没有待保存的手绘路线");
        InvalidateLocked();r.handDraft.Cancel();r.handTypeChoosing=false;r.message="已放弃本次手绘";return;
    }
    if(action=="handCancel"){
        InvalidateLocked();r.handDraft.Cancel();r.handTypeChoosing=false;r.message="已放弃本次手绘";return;
    }
    if(action=="handUndo"){
        if(!r.handDraft.Size())throw std::runtime_error("当前没有正在绘制的手绘路线");
        if(!r.handDraft.Undo())throw std::runtime_error("已经没有可以撤销的点");
        r.message="已撤销上一个点，还有 "+std::to_string(r.handDraft.Size())+" 个";
        return;
    }
    if(action=="handPoint"){
        if(!r.handDraft.Active())throw std::runtime_error("请先开始手绘路线");
        const auto x=command.value("x",std::numeric_limits<double>::quiet_NaN());
        const auto y=command.value("y",std::numeric_limits<double>::quiet_NaN());
        if(!std::isfinite(x)||!std::isfinite(y))throw std::invalid_argument("手绘点位坐标无效");
        ItemDatas point;
        const auto key=command.value("key",std::string{});
        if(!key.empty()){
            // The click landed on a point the map already knows, so the route connects to that
            // point instead of dropping a mark beside it.
            const auto item=ResolveLocked(r.scene,key);
            if(!item)throw std::invalid_argument("该点位不属于当前地图："+key);
            point=*item;
        }else if(const auto nearby=NearestCatalogPointLocked(r.scene,Coordinate(x,y),HandDrawSnapRoc)){
            // The picker only reports where the cursor was, so a press that lands on an official
            // point has to be recognised from the position — that is what makes "click a point and
            // the route connects to it" true without a second hit-test path.
            point=*nearby;
        }else{
            point.itemMapROC=Coordinate(x,y);
            point.layer.stateId=scene->kuroStateId;
            point.layer.stopKind=StopKind::Free;
        }
        if(!AutoRoute::IsFreeStop(point)) {
            const bool allowed=RoutePlanningService::HandPointAllowed(point,r.handDraft.Category());
            if(!allowed)throw std::invalid_argument("该官方点位与手绘路线类型不匹配，请选择同类型点位");
        }
        const auto stored=r.handDraft.Add(point);
        r.message=AutoRoute::IsFreeStop(stored)
            ? "已记下第 "+std::to_string(r.handDraft.Size())+" 个点（空地标记）"
            : "已连到点位："+stored.itemId;
        // The one line that answers "why did my click become a mark instead of connecting to that
        // icon?" — it carries the decision, not just the position.
        StructuredLogger::Record("info","routes","hand-drawn-point",
            "scene="+std::to_string(r.scene)+" x="+std::to_string(stored.itemMapROC.x)+" y="+std::to_string(stored.itemMapROC.y)+
            " kind="+std::string(AutoRoute::IsFreeStop(stored)?"free":"catalog")+
            " pointId="+stored.itemId+" count="+std::to_string(r.handDraft.Size()));
        return;
    }
    if(action=="handCommit"){
        if(!r.handDraft.Size())throw std::runtime_error("当前没有正在绘制的手绘路线");
        const auto name=command.value("name",std::string{"我的路线"});
        auto plan=r.handDraft.Commit(r.handDraft.RouteId(),name.empty()?std::string{"我的路线"}:name,r.profile);
        // A drawing is a route that has never been written, so it lands in the collection the
        // player is standing in — the whole point of being able to switch collections.
        plan.collection=NewRouteCollectionLocked(plan);
        r.store->Save(plan,false);
        InvalidateLocked();r.handDraft.Cancel();r.handTypeChoosing=false;
        // The drawing is saved, not started: the player asked to keep it, and quietly switching
        // what they are following would be a different decision than the one they made.
        ReloadSavedLocked();
        r.message="手绘路线已保存到「"+CollectionNameLocked(plan.collection)+"」（"+std::to_string(plan.stops.size())+" 个点），可在路线列表里应用";
        StructuredLogger::Record("info","routes","hand-drawn-committed",
            "routeId="+plan.id+" scene="+std::to_string(plan.sceneId)+" stops="+std::to_string(plan.stops.size())+
            " freeStops="+std::to_string(std::count_if(plan.stops.begin(),plan.stops.end(),
                [](const ItemDatas& item){return AutoRoute::IsFreeStop(item);}))+
            " kinds="+std::to_string(AutoRoute::Kinds(plan.stops).size()));
        return;
    }
    throw std::invalid_argument("未知的手绘操作");
}
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
    return -1;}
std::string NavigationLocked(){
    const auto& r=R();if(!r.active)return "paused";
    if(TargetIndexLocked()<0)return "finished";
    if(!r.runRequested)return "paused";
    return r.playerAvailable&&r.player.valid&&r.player.sceneId==r.active->sceneId?"navigating":"waitingForLocation";
}
Json StopJsonLocked(const ItemDatas& item,int order=0,bool skipped=false){
    const auto found=R().names.find(item.nameId);
    return {{"key",AutoRoute::Key(item)},{"stateId",item.layer.stateId},{"pointId",item.itemId},{"nameId",item.nameId},
        {"name",AutoRoute::IsFreeStop(item)?AutoRoute::FreePointName(item,order):found==R().names.end()?item.nameId:found->second},{"x",item.itemMapROC.x},{"y",item.itemMapROC.y},
        {"countryId",item.layer.countryId},{"floorId",item.layer.floorId},{"level",item.layer.level},
        {"stopKind",AutoRoute::IsFreeStop(item)?"free":"catalog"},{"routeId",item.freeRouteId},
        {"freeIcon",AutoRoute::IconId(item.freeIcon)},{"freeCategory",AutoRoute::CategoryId(item.freeCategory)},
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
        {"stops",std::move(stops)},{"planarLength",length},{"handDrawn",plan.handDrawn},
        {"routeCategory",AutoRoute::CategoryId(plan.routeCategory)},{"legacyHandDrawn",plan.legacyHandDrawn},
        {"filterByRoute",plan.filterByRoute}};
}
// The route the interface shows at the top of the list: the one being followed, or — while nothing
// is being followed — the preview the player generated or the drawing they are making. Saving and
// deleting act on this one object, so the "current route" has to be a single answer, not two.
// It is emitted as the plan itself (or null) so both sides agree on the shape; whether it is a
// preview rides in its own field of the snapshot.
Json CurrentRouteLocked(const std::string& previewId){
    const auto& r=R();
    if(r.active)return PlanJsonLocked(*r.active);
    const auto it=r.drafts.find(r.scene);
    if(previewId.empty()||it==r.drafts.end()||!it->second.preview)return nullptr;
    return PlanJsonLocked(*it->second.preview);
}
bool CurrentRouteIsPreviewLocked(){
    const auto& r=R();
    if(r.active)return false;
    // A drawing that was finished but not saved is not a "preview" either: the interface labels it
    // as an unsaved drawing and offers to save or discard it.
    const auto it=r.drafts.find(r.scene);
    return it!=r.drafts.end()&&it->second.preview.has_value()&&!r.handDraft.Pending();
}
// A drawing in progress is handed to the renderer as a plan so it is drawn by the exact same code
// as every other route: same colour, same width, same numbered stop badges, same start marker.
AutoRoute::Plan HandDraftPlanLocked(){
    const auto& r=R();
    AutoRoute::Plan plan;
    plan.id=HandDraftId;plan.name="手绘路线";plan.profileId=r.profile;plan.sceneId=r.handDraft.SceneId();
    plan.handDrawn=true;plan.routeCategory=r.handDraft.Category();plan.legacyHandDrawn=false;
    // A finished-but-unsaved drawing is a draft too: it is still the thing being edited, drawn as the
    // solid arrowed path rather than as a planned route waiting for confirmation.
    plan.handDraft=true;
    if(r.handDraft.Size())plan.start={plan.sceneId,r.handDraft.Points().front().itemMapROC,"manual",0,0,true};
    plan.stops=r.handDraft.Points();
    return plan;
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
        {"farmMode",r.farmMode},{"farmNotice",r.farmNotice},{"farmNoticeSerial",r.farmNoticeSerial},
        {"orderRevision",r.orderRevision},{"previousTarget",r.previousTarget?StopJsonLocked(*r.previousTarget):Json(nullptr)},
        {"currentRoute",CurrentRouteLocked(HandDraftId)},
        {"currentRouteIsPreview",CurrentRouteIsPreviewLocked()},
        {"handDrawnActive",r.handDraft.Active()},{"handDrawnPending",r.handDraft.Pending()},
        {"handDrawnCount",r.handDraft.Size()},{"handDrawnTypeChoosing",r.handTypeChoosing},
        {"handCategory",AutoRoute::CategoryId(r.handDraft.Category())},{"handIcon",AutoRoute::IconId(r.handDraft.Icon())},
        {"currentCollection",r.currentCollection},{"collections",CollectionsJsonLocked()},
        {"transfer",r.transfer},
        {"savedRoutes",SavedRoutesJsonLocked()}};
}
void Emit(){
    auto& r=R();std::function<void(const Json&)> callback;
    {std::scoped_lock lock(r.eventMutex);callback=r.callback;}
    if(!callback)return;
    Json state;{std::scoped_lock lock(r.mutex);state=SnapshotLocked();}
    callback({{"type","routePlanningChanged"},{"data",std::move(state)}});
}
void SyncProfileLocked(){
    auto& r=R();const auto profile=DrawItemBase::MarkerProfile();
    if(profile==r.profile) {
        if(r.deletionRecoveryPending) {
            try {r.store->RecoverDeletions(profile,[&](const auto& id){DrawItemBase::RemoveFreePointCompletions(profile,id);});r.deletionRecoveryPending=false;ReloadSavedLocked();RefreshCompletedLocked();}
            catch(const std::exception& e){r.message=std::string("自由点删除清理待重试：")+e.what();}
        }
        return;
    }
    InvalidateLocked();r.profile=profile;r.enabled=false;r.pendingNew=false;r.drafts.clear();r.active.reset();r.skipHistory.clear();
    InvalidateAutoLocked(true);r.stablePlayer.Reset();r.hasAutoPosition=false;
    r.farmMode=false;r.farmConfirmation.Reset();r.farmNotice.clear();
    // The other profile's collections must not survive the switch even if reading the new index
    // fails below: a list built from the wrong profile's collections would look like data loss.
    r.collections={};r.currentCollection=AutoRoute::DefaultCollectionId;r.saved=Json::array();r.deletionRecoveryPending=true;
    // A package inspected under one profile must never be applied under another.
    r.transfer=nullptr;
    r.handDraft.Cancel();r.handTypeChoosing=false;
    r.mapSuspended=false;
    r.runRequested=false;r.completed.clear();r.message.clear();r.tool="pan";
    try {ReloadCollectionsLocked();ReloadSavedLocked();
        r.store->RecoverDeletions(profile,[&](const auto& id){DrawItemBase::RemoveFreePointCompletions(profile,id);});r.deletionRecoveryPending=false;
        r.active=r.store->LoadActive(profile,ResolveLocked);
        if(r.active){ValidateTypedRoute(*r.active);r.skipHistory=r.active->skipHistory;r.message="已恢复自动路线，点击继续导航";
            // A restored route carries its own farming setting; it stays off until the player
            // resumes, because the mode itself only acts while the navigation is running.
            r.farmMode=r.active->farmMode;}}
    catch(const std::exception& e){r.message=std::string("自动路线暂停：")+e.what();r.active.reset();}
    r.farmConfirmation.Reset();r.farmNotice.clear();
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
    r.pending=Job{r.epoch.load(),r.profile,r.scene,draft.start,std::move(targets),
        replan&&r.active?r.active->farmMode:false,
        replan&&r.active?AutoRoute::NormalizeCollectionId(r.active->collection):std::string{}};r.wake.notify_one();
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
                    plan.start=job.start;plan.stops=result.stops;plan.farmMode=job.farmMode;plan.collection=job.collection;
                    AutoRoute::ScopeFreePoints(plan,plan.id);
                    r.drafts[job.scene].preview=std::move(plan);r.message="路线预览已生成，点击开始导航";}
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
    auto& r=R();const std::array<const Json*,9> sources={&DrawItemBase::itemsJsonData_World,&DrawItemBase::itemsJsonData_Tethys,
        &DrawItemBase::itemsJsonData_Fabricatorium,&DrawItemBase::itemsJsonData_Avinoleum,&DrawItemBase::itemsJsonData_Lahai,
        &DrawItemBase::itemsJsonData_LowerVault,&DrawItemBase::itemsJsonData_Darkplain,&DrawItemBase::itemsJsonData_TimeRiftRuins,
        &DrawItemBase::itemsJsonData_MengshuTianluo};
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
        r.store=std::make_unique<AutoRoute::RoutePlanStore>(StructuredLogger::ApplicationDataDirectory()/"SavedRoutes");
        r.collectionStore=std::make_unique<AutoRoute::RouteCollections>(StructuredLogger::ApplicationDataDirectory()/"SavedRoutes");
        BuildCatalogLocked();r.stopping=false;r.ready=true;}
    // Routes drawn by the old hand tool are imported once, before anything reads the store. The
    // catalogue is already built, so an endpoint that lands on a known point keeps its type.
    {
        const auto root=StructuredLogger::ApplicationDataDirectory()/"SavedRoutes";
        // A portable install keeps its routes beside the program. The retired hand-drawing class
        // used to copy them in as a side effect of preparing its storage; the copy has to survive
        // that removal, otherwise those players' routes are never seen by the import below.
        try{
            std::error_code error;
            // The program's own folder, not the process working directory: a portable install keeps
            // its SavedRoutes next to the exe.
            char programPath[MAX_PATH]{};
            const auto programLength=GetModuleFileNameA(nullptr,programPath,MAX_PATH);
            const auto portable=programLength?std::filesystem::path(std::string(programPath,programLength)).parent_path()/"SavedRoutes"
                :std::filesystem::path{};
            if(!portable.empty()&&std::filesystem::exists(portable,error))
                for(const auto& entry:std::filesystem::directory_iterator(portable,error)){
                    if(error)break;
                    if(!entry.is_regular_file()||entry.path().extension()!=".json")continue;
                    const auto destination=root/entry.path().filename();
                    if(std::filesystem::exists(destination))continue;
                    std::filesystem::create_directories(root,error);
                    std::filesystem::copy_file(entry.path(),destination,
                        std::filesystem::copy_options::skip_existing,error);
                }
        }catch(const std::exception& error){
            StructuredLogger::Record("warn","routes","portable-route-copy-failed",error.what());
        }
        const auto profile=DrawItemBase::MarkerProfile();
        const auto lookupScene=[](const std::string& name){return Scene::SceneNameToId(name.c_str());};
        const auto lookupPoint=[](int scene,const Coordinate& position)->std::optional<ItemDatas>{
            auto& runtime=R();const auto sceneIt=runtime.catalog.find(scene);
            if(sceneIt==runtime.catalog.end())return {};
            const ItemDatas* best=nullptr;double bestDistance=0;
            for(const auto& [key,item]:sceneIt->second){
                const double distance=std::hypot(item.itemMapROC.x-position.x,item.itemMapROC.y-position.y);
                if(!best||distance<bestDistance){best=&item;bestDistance=distance;}
            }
            // The caller applies the tolerance; this only answers "which point is closest".
            return best?*best:std::optional<ItemDatas>{};
        };
        // The import has to land in the profile the player will actually be using, and that is not
        // knowable here: the marker profile at startup can differ from the one selected later, and
        // a route saved under a profile nobody opens is a route the player has lost. So import into
        // every profile that already has routes (those are proven-in-use), plus the one in hand and
        // the default. The source file is archived only once all of them succeeded.
        std::vector<std::string> profiles;
        const auto addProfile=[&](const std::string& value){
            if(!value.empty()&&std::find(profiles.begin(),profiles.end(),value)==profiles.end())profiles.push_back(value);};
        try{
            std::error_code error;
            const auto autoRoot=root/"Auto";
            if(std::filesystem::exists(autoRoot,error))
                for(const auto& entry:std::filesystem::directory_iterator(autoRoot,error)){
                    if(error)break;
                    if(entry.is_directory())addProfile(entry.path().filename().string());
                }
        }catch(const std::exception& error){
            StructuredLogger::Record("warn","routes","legacy-route-profile-scan-failed",error.what());
        }
        addProfile(profile);addProfile("local");
        try{
            for(const auto& target:profiles){
                AutoRoute::LegacyHandRouteFiles::Import(root,target,lookupScene,lookupPoint,
                    [&](const std::string& line){StructuredLogger::Record("info","routes",line+" profile="+target,"");});
            }
        }catch(const std::exception& error){
            StructuredLogger::Record("warn","routes","legacy-route-import-failed",error.what());
        }
    }
    {std::scoped_lock lock(r.mutex);SyncProfileLocked();}
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
                {"stopKind",AutoRoute::IsFreeStop(item)?"free":"catalog"},{"routeId",item.freeRouteId},
                {"freeIcon",AutoRoute::IconId(item.freeIcon)},{"freeCategory",AutoRoute::CategoryId(item.freeCategory)},
                {"localName",AutoRoute::IsFreeStop(item)?AutoRoute::FreePointName(item,target+1):std::string{}},
                {"countryId",item.layer.countryId},{"floorId",item.layer.floorId},{"level",item.layer.level},
                {"completed",false},{"screenX",x},{"screenY",y}};
        }
        return {{"accepted",true},{"data",{{"profileId",r.profile},{"routeId",r.active?r.active->id:std::string{}},
            {"revision",r.revision},{"navigationStatus",NavigationLocked()},{"selection",std::move(selection)}}}};
    }catch(const std::exception& e){return {{"accepted",false},{"message",e.what()},{"data",Json::object()}};}
}
bool RoutePlanningService::HandPointAllowed(const ItemDatas& point,FreePointCategory category) {
    return AutoRoute::IsFreeStop(point) || (category==FreePointCategory::Collectible ?
        DrawItemBase::IsCollectiblePoint(point.nameId) : DrawItemBase::IsRefreshablePoint(point.nameId));
}
std::optional<ItemDatas> RoutePlanningService::HandPointCandidate(int scene,const Coordinate& roc,const std::string& key) {
    auto& r=R();std::scoped_lock lock(r.mutex);
    if(!r.ready||!Scene::IsKnown(scene)||!Finite(roc))return {};
    return key.empty()?NearestCatalogPointLocked(scene,roc,HandDrawSnapRoc):ResolveLocked(scene,key);
}
nlohmann::json RoutePlanningService::CompleteFreePoint(const Json& command){
    auto& r=R();
    try {
        ItemDatas item;std::string profile;Json result;
        {
            std::scoped_lock lock(r.mutex);
            if(!r.ready||command.value("profileId",std::string{})!=r.profile||!r.active||
                command.value("routeId",std::string{})!=r.active->id)
                throw std::runtime_error("路线或档案已变化，请重新选择自由点");
            const auto found=std::find_if(r.active->stops.begin(),r.active->stops.end(),[&](const auto& stop){
                return AutoRoute::IsFreeStop(stop)&&stop.itemId==command.value("pointId",std::string{})&&
                    stop.layer.stateId==command.value("stateId",0);
            });
            if(found==r.active->stops.end())throw std::runtime_error("自由点不属于当前路线");
            if(command.value("automatic",false)&&(!r.runRequested||!r.farmMode||found->freeCategory!=FreePointCategory::Daily))
                throw std::runtime_error("该自由点不能自动完成");
            item=*found;profile=r.profile;
            // Persist while the route lock still owns the validated identity. The notification
            // is published after unlock because it refreshes this same route state.
            result=DrawItemBase::SetFreePointCompletion(profile,item,command.value("completed",true));
        }
        if(result.value("accepted",false)) {
            Json event={{"type","markerFreeCompletionChanged"},{"profileId",profile},{"source","local"},{"point",result.at("data").at("point")}};
            for(const auto* field:{"guideSelectionGeneration","guideWindowHwnd"})if(command.contains(field))event[field]=command.at(field);
            DrawItemBase::PublishMarkerEvent(std::move(event));
        }
        return result;
    }catch(const std::exception& error){return {{"accepted",false},{"message",error.what()},{"data",Json::object()}};}
}
RoutePlanningView RoutePlanningService::View(){
    auto& r=R();std::scoped_lock lock(r.mutex);RoutePlanningView v;
    v.enabled=r.enabled;v.computing=r.computing;v.profileId=r.profile;v.tool=r.tool;v.message=r.message;v.sceneId=r.scene;
    v.revision=r.revision;v.generation=r.epoch.load();v.hiddenCount=HiddenLocked();v.active=r.active;v.completed=r.completed;
    v.mapStart=r.mapStart;
    v.autoReplanEnabled=r.autoEnabled;v.autoReplanComputing=r.autoComputing;v.autoReplanStatus=r.autoStatus;
    v.farmMode=r.farmMode;v.farmNotice=r.farmNotice;v.farmNoticeSerial=r.farmNoticeSerial;
    v.orderRevision=r.orderRevision;v.previousTarget=r.previousTarget;
    v.currentTargetIndex=TargetIndexLocked();v.navigationStatus=NavigationLocked();v.navigating=v.navigationStatus=="navigating";
    const auto it=r.drafts.find(r.scene);if(it!=r.drafts.end()){v.selected=it->second.selected;v.start=it->second.start;v.preview=it->second.preview;}
    if(r.handDraft.Size()){v.handDraftPreview=HandDraftPlanLocked();v.handDraft=v.handDraftPreview;}
    v.handDrawnActive=r.handDraft.Active();v.handDrawnPending=r.handDraft.Pending();
    v.handDrawnSceneId=r.handDraft.SceneId();
    v.handDrawnTypeChoosing=r.handTypeChoosing;v.handCategory=r.handDraft.Category();v.handIcon=r.handDraft.Icon();
    v.handDrawnCount=r.handDraft.Size();
    return v;
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
    // A drawing in progress is shown by the preview rules so it is drawn like every other route; it
    // is not gated on the selection mode, because drawing by hand never enters that mode. A drawing
    // that was finished but not yet saved stays on the map for the same reason: the player is about
    // to decide whether to keep it, and it has to still be there while they decide.
    if(r.handDraft.Size()>0)result.previewId=HandDraftId;
    result.navigating=NavigationLocked()=="navigating";return result;
}
bool RoutePlanningService::PlanningMode(){std::scoped_lock lock(R().mutex);return R().enabled||R().handDraft.Active();}
void RoutePlanningService::ObserveMap(int scene,const std::vector<ItemDatas>& visible){
    auto& r=R();bool changed=false;
    {std::scoped_lock lock(r.mutex);if(!r.ready||!Scene::IsKnown(scene))return;
        const auto before=HiddenLocked();
        if(r.observedScene!=scene){r.observedScene=scene;
            if(r.mapSuspended){
                r.mapSuspended=false;changed=true;
                StructuredLogger::Record("info","routes","map-context-resumed",
                    "reason=fresh-map scene="+std::to_string(scene)+" draftScene="+std::to_string(r.handDraft.SceneId())+
                    " points="+std::to_string(r.handDraft.Size()));
            }
            if(r.scene!=scene){InvalidateLocked();r.scene=scene;r.tool="pan";changed=true;}}
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
        r.handDraft.Cancel();r.handTypeChoosing=false;
        r.mapSuspended=false;
        r.message="游戏定位已停止，路线保留";}Emit();
}
void RoutePlanningService::MapUnavailable(){
    auto& r=R();bool changed=false;{
        std::scoped_lock lock(r.mutex);if(!r.ready)return;
        if(r.observedScene){
            StructuredLogger::Record("info","routes","map-context-suspended",
                "reason=render-unavailable scene="+std::to_string(r.observedScene)+
                " points="+std::to_string(r.handDraft.Size()));
            r.observedScene=0;r.visible.clear();r.visibleKeys.clear();r.mapSuspended=true;
            // Revoke old input generations and pending work, not the player's draft.
            InvalidateLocked();changed=true;
        }
    }if(changed)Emit();
}
void RoutePlanningService::MapClosed(){
    auto& r=R();bool changed=false;{
        std::scoped_lock lock(r.mutex);if(!r.ready)return;
        // Suspension already clears observedScene. A subsequent real closure
        // must still cancel active drawing, including an empty drawing.
        if(r.observedScene||r.mapSuspended||r.handDraft.Active()){
            StructuredLogger::Record("info","routes","map-context-closed",
                "reason=confirmed-gameplay scene="+std::to_string(r.scene)+
                " points="+std::to_string(r.handDraft.Size()));
            r.observedScene=0;r.visible.clear();r.visibleKeys.clear();r.mapSuspended=false;
            InvalidateLocked();
            if(r.handDraft.Active()){r.handDraft.Cancel();r.message="已离开大地图，本次手绘已取消";}
            changed=true;
        }
    }if(changed)Emit();
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
    auto& r=R();bool changed=false;
    std::vector<ItemDatas> reached;std::string reachedProfile;
    {
        std::unique_lock lock(r.mutex);if(!r.ready)return;
        // An observation for a superseded target cannot erase or replace its successor's evidence.
        const int target=TargetIndexLocked();
        if(!r.active||target<0||observation.profileId!=r.profile||observation.routeId!=r.active->id||
            observation.orderRevision!=r.orderRevision||observation.targetKey!=AutoRoute::Key(r.active->stops[target]))return;
        r.proximity=observation;const auto now=Clock::now();
        r.proximity.valid=r.proximity.valid&&r.stablePlayer.last.Fresh(now)&&ProximityValidLocked(now);
        if(r.nearConfirmation.Observe(r.proximity,now)&&r.previousTarget){
            r.previousTarget.reset();++r.orderRevision;++r.revision;r.proximity={};r.nearConfirmation.Reset();changed=true;
        }
        // Farming mode marks the targets the player has actually reached: the current one and
        // any later target of this route that lies in the same cluster. Only the daily-refresh
        // categories take part — a one-off collectible marked here would be consumed without
        // the player ever picking it up, and no reset can give it back.
        if(r.farmMode&&r.runRequested&&r.proximity.valid){
            std::vector<std::string> inRange;
            for(const auto& entry:observation.nearby){
                if(!std::isfinite(entry.distancePixels)||entry.distancePixels>=FarmMode::Range::Pixels())continue;
                const auto found=std::find_if(r.active->stops.begin(),r.active->stops.end(),
                    [&](const ItemDatas& item){return AutoRoute::Key(item)==entry.key;});
                if(found==r.active->stops.end())continue;
                if(AutoRoute::IsFreeStop(*found)?found->freeCategory!=FreePointCategory::Daily:
                    !DrawItemBase::IsRefreshablePoint(found->nameId)&&!DrawItemBase::IsRefreshablePointId(found->itemId))continue;
                if(r.completed.contains(entry.key)||r.active->skipped.contains(entry.key))continue;
                inRange.push_back(entry.key);
            }
            for(const auto& key:r.farmConfirmation.Observe(inRange,now)){
                const auto found=std::find_if(r.active->stops.begin(),r.active->stops.end(),
                    [&](const ItemDatas& item){return AutoRoute::Key(item)==key;});
                if(found!=r.active->stops.end())reached.push_back(*found);
            }
            if(!reached.empty())reachedProfile=r.profile;
        }else r.farmConfirmation.Reset();
    }
    if(!reached.empty()){
        // The store takes its own lock and publishing a completion re-enters this service,
        // so the write happens with our lock released — the same shape "complete current
        // target" already uses.
        Json points=Json::array();
        std::size_t freeCount=0;
        for(const auto& item:reached) {
            if(AutoRoute::IsFreeStop(item)) {
                const auto done=CompleteFreePoint({{"profileId",reachedProfile},{"routeId",item.freeRouteId},
                    {"stateId",item.layer.stateId},{"pointId",item.itemId},{"completed",true},{"automatic",true}});
                if(done.value("accepted",false))++freeCount;
            } else points.push_back({{"stateId",item.layer.stateId},{"pointId",item.itemId},{"nameId",item.nameId}});
        }
        const auto result=DrawItemBase::HandleMarkerCommand({{"type","markerFarmComplete"},
            {"profileId",reachedProfile},{"points",std::move(points)}});
        const auto count=freeCount+(result.value("accepted",false)?result.at("data").value("changed",std::size_t{}):std::size_t{});
        {
            std::scoped_lock lock(r.mutex);
            // The route has already refreshed itself through OnMarkerChanged; this only
            // carries the sentence back to the toolbar.
            if(r.farmMode&&count){
                r.farmNotice="刷怪采集模式自动标记 "+std::to_string(count)+" 个";
                ++r.farmNoticeSerial;++r.revision;
            }
        }
        Emit();
        return;
    }
    if(changed)Emit();
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
        // `switch` names a route the player picked from the list, which by definition is usually not
        // the one being followed; it belongs with `load` and `delete` on the exempt side of the
        // fence, or choosing a route while another one is active would always be refused.
        if(command.contains("routeId")&&action!="load"&&action!="delete"&&action!="switch"&&(!r.active||command.at("routeId")!=r.active->id))
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
            // A preview has never been on disk, so this is the moment it becomes a real route and
            // the moment it takes the collection the player is saved it from: the current one for
            // a fresh plan, the source route's own for a replan.
            auto plan=*draft.preview;
            plan.collection=NewRouteCollectionLocked(plan);
            RefreshCompletedLocked();r.store->Save(plan,true);r.active=plan;r.skipHistory.clear();r.runRequested=true;r.enabled=false;r.tool="pan";
            // The route brings its own farming setting: a route built to sweep monsters and
            // herbs switches the mode on by itself every time it is started again.
            r.farmMode=r.active->farmMode;r.farmConfirmation.Reset();r.farmNotice.clear();
            InvalidateAutoLocked(true);r.hasAutoPosition=false;
            draft.preview.reset();
            ReloadSavedLocked();r.message="自动路线已保存到「"+CollectionNameLocked(plan.collection)+"」并开始导航";
        }else if(action=="stop"){
            // Persist the exit before changing runtime state. A failed write
            // leaves the previous route usable and reports the failure.
            r.store->ClearActive(r.profile);StopNavigationLocked();
            RefreshCompletedLocked();r.message="已退出导航并隐藏路线，保存的路线和完成记录保留";
        }else if(action=="delete"){
            const auto id=command.at("routeId").get<std::string>();
            const bool removingActive=r.active&&AutoRoute::SameRouteId(r.active->id,id);
            try {DeleteRouteLocked(id);}
            catch(const AutoRoute::DeleteRollbackFailure&){
                if(removingActive)StopNavigationLocked();
                ReloadSavedLocked();RefreshCompletedLocked();
                throw;
            }
            ForgetDeletedRouteLocked(id);
            RefreshCompletedLocked();ReloadSavedLocked();
            r.message=removingActive?"路线已删除并退出导航，自由点完成记录已清理，官方点位记录保留":"路线已删除，自由点完成记录已清理，官方点位记录保留";
        }else if(action=="collectionNew"){
            const auto name=AutoRoute::RouteCollections::TrimName(command.value("name",std::string{}));
            if(!AutoRoute::RouteCollections::IsValidName(name))
                throw std::invalid_argument("合集名字要在 1–40 个字之间");
            if(AutoRoute::SameRouteId(name,AutoRoute::DefaultCollectionName))
                throw std::invalid_argument(std::string("「")+AutoRoute::DefaultCollectionName+"」是保留名字，请换一个");
            // Creating a name that is already taken is refused rather than silently numbered: the
            // player typed a name, and answering "that one exists" is the only honest reply.
            if(AutoRoute::RouteCollections::FindByName(r.collections,name))
                throw std::runtime_error("已经有叫「"+name+"」的合集了，请换个名字");
            AutoRoute::Collection created;
            created.id=NewId();created.name=name;created.createdUnixMs=NowUnixMs();
            r.collections.collections.push_back(created);
            // Creating a collection is also entering it: the player asked for a place to put
            // routes, and the next route they save is the one they are thinking about.
            r.currentCollection=created.id;
            SaveCollectionsLocked();
            r.message="已创建合集「"+name+"」，之后保存的路线会放进这里";
            StructuredLogger::Record("info","routes","collection-created",
                "collectionId="+created.id+" name="+name);
        }else if(action=="collectionRename"){
            const auto id=command.at("collectionId").get<std::string>();
            if(AutoRoute::IsDefaultCollection(id))throw std::invalid_argument("默认合集的名字不能改");
            auto* found=MutableCollectionLocked(id);
            if(!found)throw std::runtime_error("要改名的合集不存在");
            const auto name=AutoRoute::RouteCollections::TrimName(command.value("name",std::string{}));
            if(!AutoRoute::RouteCollections::IsValidName(name))
                throw std::invalid_argument("合集名字要在 1–40 个字之间");
            if(AutoRoute::SameRouteId(name,AutoRoute::DefaultCollectionName))
                throw std::invalid_argument(std::string("「")+AutoRoute::DefaultCollectionName+"」是保留名字，请换一个");
            if(const auto* clash=AutoRoute::RouteCollections::FindByName(r.collections,name);
                clash&&!AutoRoute::SameRouteId(clash->id,found->id))
                throw std::runtime_error("已经有叫「"+name+"」的合集了，请换个名字");
            const auto before=found->name;
            found->name=name;
            SaveCollectionsLocked();
            r.message="合集「"+before+"」已改名为「"+name+"」";
        }else if(action=="collectionDelete"){
            const auto id=command.at("collectionId").get<std::string>();
            if(AutoRoute::IsDefaultCollection(id))throw std::invalid_argument("默认合集不能删除");
            const auto* found=AutoRoute::RouteCollections::Find(r.collections,AutoRoute::NormalizeCollectionId(id));
            if(!found)throw std::runtime_error("要删除的合集不存在");
            const auto name=found->name,collectionId=found->id;
            // Deleting a collection takes its routes with it — that is the rule the player picked.
            // The victims are collected from the rows the interface is showing, so what is deleted
            // is exactly what the confirmation counted.
            std::vector<std::string> doomed;
            for(const auto& row:r.saved)
                if(AutoRoute::SameRouteId(row.value("collection",std::string{AutoRoute::DefaultCollectionId}),collectionId))
                    doomed.push_back(row.value("id",std::string{}));
            const bool removingActive=r.active&&std::any_of(doomed.begin(),doomed.end(),
                [&](const std::string& routeId){return AutoRoute::SameRouteId(r.active->id,routeId);});
            std::size_t removed=0;std::string failure;
            for(const auto& routeId:doomed){
                try {DeleteRouteLocked(routeId);ForgetDeletedRouteLocked(routeId);++removed;}
                catch(const AutoRoute::DeleteRollbackFailure&){
                    if(r.active&&AutoRoute::SameRouteId(r.active->id,routeId))StopNavigationLocked();
                    ReloadSavedLocked();RefreshCompletedLocked();
                    throw;
                }catch(const std::exception& error){if(failure.empty())failure=error.what();}
            }
            if(!failure.empty()){
                // A collection that could not be emptied keeps its name and its remaining routes.
                // Saying so beats reporting a deletion that did not happen to everything.
                ReloadSavedLocked();
                throw std::runtime_error("合集「"+name+"」里有路线没能删除（"+failure+"），已删除 "+
                    std::to_string(removed)+" 条，合集仍然保留");
            }
            if(removingActive)StopNavigationLocked();
            if(AutoRoute::SameRouteId(r.currentCollection,collectionId))r.currentCollection=AutoRoute::DefaultCollectionId;
            r.collections.collections.erase(std::remove_if(r.collections.collections.begin(),r.collections.collections.end(),
                [&](const AutoRoute::Collection& item){return AutoRoute::SameRouteId(item.id,collectionId);}),
                r.collections.collections.end());
            SaveCollectionsLocked();
            ReloadSavedLocked();
            RefreshCompletedLocked();
            r.message="已删除合集「"+name+"」和它里面的 "+std::to_string(removed)+" 条路线，自由点完成记录已清理，官方点位记录保留";
            StructuredLogger::Record("info","routes","collection-deleted",
                "collectionId="+collectionId+" name="+name+" routes="+std::to_string(removed));
        }else if(action=="collectionCurrent"){
            const auto id=AutoRoute::NormalizeCollectionId(command.at("collectionId").get<std::string>());
            if(!AutoRoute::RouteCollections::Exists(r.collections,id))
                throw std::runtime_error("要切换的合集不存在");
            if(!AutoRoute::SameRouteId(id,r.currentCollection)){
                r.currentCollection=id;
                SaveCollectionsLocked();
            }
            r.message="已切换到合集「"+CollectionNameLocked(id)+"」，之后保存的路线会放进这里";
        }else if(action=="routeCollection"){
            // `routeIds`, never `routeId`: the pre-action fence refuses any command that names a
            // route other than the active one, and moving a batch out of the list is exactly that.
            // (`switch` was refused in production once for missing the exemption.)
            const auto ids=command.at("routeIds").get<std::vector<std::string>>();
            if(ids.empty())throw std::invalid_argument("请先选择要移动的路线");
            const auto target=AutoRoute::NormalizeCollectionId(command.at("collectionId").get<std::string>());
            if(!AutoRoute::RouteCollections::Exists(r.collections,target))
                throw std::runtime_error("要移动到的合集不存在");
            std::size_t moved=0;std::string failure;
            for(const auto& routeId:ids){
                try {
                    auto plan=r.store->Load(r.profile,routeId,ResolveLocked);
                    if(AutoRoute::SameRouteId(AutoRoute::NormalizeCollectionId(plan.collection),target))continue;
                    plan.collection=target;
                    // `false`: moving a route must not move the active pointer. Whether it happens
                    // to be the route being followed is none of this operation's business.
                    r.store->Save(plan,false);
                    // The in-memory copy has to follow, or saving the active route afterwards would
                    // write the old collection back over this move.
                    if(r.active&&AutoRoute::SameRouteId(r.active->id,routeId))r.active=plan;
                    ++moved;
                }catch(const std::exception& error){if(failure.empty())failure=error.what();}
            }
            ReloadSavedLocked();
            if(!failure.empty())
                throw std::runtime_error("有路线没能移动（"+failure+"），已移动 "+std::to_string(moved)+" 条");
            r.message="已把 "+std::to_string(moved)+" 条路线移到「"+CollectionNameLocked(target)+"」";
        }else if(action=="export"){
            // `routeIds` for a hand-picked batch and `collectionId` for a whole collection: never
            // `routeId`, which the pre-action fence would refuse for every route but the active one.
            if(!command.contains("path"))throw std::invalid_argument("请先选择要保存的位置");
            const auto path=AutoRoute::Utf8Path(command.at("path").get<std::string>());
            const bool wholeCollection=command.contains("collectionId");
            std::vector<AutoRoute::Plan> plans;std::string target;
            std::size_t unreadable=0;
            if(wholeCollection){
                const auto id=AutoRoute::NormalizeCollectionId(command.at("collectionId").get<std::string>());
                if(!AutoRoute::RouteCollections::Exists(r.collections,id))throw std::runtime_error("要导出的合集不存在");
                target=CollectionNameLocked(id);
                for(const auto& row:r.saved){
                    if(!AutoRoute::SameRouteId(row.value("collection",std::string{AutoRoute::DefaultCollectionId}),id))continue;
                    // A route whose file is damaged, or whose points no longer resolve against the
                    // current resources, is skipped rather than failing the export: the player is
                    // asking to keep what they have, and one bad row is not a reason to hand them
                    // nothing. The count is reported so the gap is never silent.
                    if(row.value("corrupt",false)){++unreadable;continue;}
                    try {plans.push_back(r.store->Load(r.profile,row.value("id",std::string{}),ResolveLocked));}
                    catch(const std::exception&){++unreadable;}
                }
                if(plans.empty())throw std::runtime_error("合集「"+target+"」里没有能导出的路线");
            }else{
                const auto ids=command.at("routeIds").get<std::vector<std::string>>();
                if(ids.empty())throw std::invalid_argument("请先选择要导出的路线");
                for(const auto& routeId:ids){
                    try {plans.push_back(r.store->Load(r.profile,routeId,ResolveLocked));}
                    catch(const std::exception&){++unreadable;}
                }
                if(plans.empty())throw std::runtime_error("选中的路线都读不出来，无法导出");
            }
            AutoRoute::RouteBundle::Save(path,AutoRoute::RouteBundle::Write(wholeCollection,target,plans));
            r.message="已导出 "+std::to_string(plans.size())+" 条路线到「"+
                AutoRoute::Utf8Text(path.filename())+"」"+
                (unreadable? "（有 "+std::to_string(unreadable)+" 条读不出来，没有导出）":"");
            StructuredLogger::Record("info","routes","route-bundle-exported",
                "kind="+std::string(wholeCollection?"collection":"routes")+" routes="+std::to_string(plans.size())+
                " skipped="+std::to_string(unreadable)+" path="+AutoRoute::Utf8Text(path));
        }else if(action=="importInspect"){
            // Reading only. The player is shown what the file holds — and, for a collection whose
            // name is already taken, what the choice would do — before a single byte is written.
            const auto path=AutoRoute::Utf8Path(command.at("path").get<std::string>());
            // Whatever was pending is the answer to the previous file, and this is a different
            // question. Dropping it first also means a file that cannot be read leaves nothing
            // behind that could still be applied.
            r.transfer=nullptr;
            const auto bundle=AutoRoute::RouteBundle::Load(path);
            std::size_t skipped=0;
            const auto plans=ParseBundledRoutesLocked(bundle,skipped);
            if(plans.empty())
                throw std::runtime_error("这个路线包里没有能导入的路线"+(skipped?"（"+std::to_string(skipped)+" 条读不出来）":""));
            Json conflict=nullptr;
            if(bundle.collection)
                if(const auto* clash=AutoRoute::RouteCollections::FindByName(r.collections,bundle.collectionName))
                    conflict={{"collectionId",clash->id},{"name",clash->name},
                        {"routeCount",CollectionRouteCountLocked(clash->id)}};
            r.transfer={{"kind",bundle.collection?"collection":"routes"},
                {"path",command.at("path").get<std::string>()},
                {"collectionName",bundle.collectionName},
                {"collectionId",conflict.is_null()?std::string{}:conflict.at("collectionId").get<std::string>()},
                {"routeCount",plans.size()},{"skipped",skipped},{"conflict",std::move(conflict)}};
            r.message=bundle.collection
                ? "路线包「"+bundle.collectionName+"」里有 "+std::to_string(plans.size())+" 条路线"
                : "路线包里有 "+std::to_string(plans.size())+" 条路线，将导入当前合集「"+CollectionNameLocked(r.currentCollection)+"」";
            StructuredLogger::Record("info","routes","route-bundle-inspected",
                "kind="+std::string(bundle.collection?"collection":"routes")+" routes="+std::to_string(plans.size())+
                " skipped="+std::to_string(skipped)+" path="+AutoRoute::Utf8Text(path));
        }else if(action=="importApply"){
            const auto path=AutoRoute::Utf8Path(command.at("path").get<std::string>());
            const auto mode=command.value("mode",std::string{});
            // Applying means the player was already shown this file: the inspection is what carried
            // the kind, the name and the conflict, and re-deciding them here would be a second,
            // quieter decision made on their behalf.
            if(!r.transfer.is_object()||r.transfer.value("path",std::string{})!=command.at("path").get<std::string>())
                throw std::runtime_error("请先选择要导入的路线包");
            if(mode!="routes"&&mode!="collectionNew"&&mode!="collectionOverwrite")
                throw std::invalid_argument("导入方式无效");
            const auto bundle=AutoRoute::RouteBundle::Load(path);
            if(bundle.collection!=(mode!="routes"))
                throw std::invalid_argument(bundle.collection?"这是合集路线包，只能按合集导入":"这是路线包，只能按路线导入");
            // Everything is read and validated before anything is written. Only then does the one
            // destructive branch — replacing a collection — delete what is already there, so a
            // bundle that cannot be imported never costs the player the collection it collided with.
            std::size_t skipped=0;
            const auto plans=ParseBundledRoutesLocked(bundle,skipped);
            if(plans.empty())throw std::runtime_error("这个路线包里没有能导入的路线，已放弃导入");
            std::string target;
            AutoRoute::Collection created;
            if(mode=="routes"){
                target=r.currentCollection;
            }else if(mode=="collectionNew"){
                created.id=NewId();
                created.name=AutoRoute::RouteCollections::UniqueName(r.collections,bundle.collectionName);
                created.createdUnixMs=NowUnixMs();
                target=created.id;
                r.collections.collections.push_back(created);
            }else{
                const auto* clash=AutoRoute::RouteCollections::FindByName(r.collections,bundle.collectionName);
                if(!clash)throw std::runtime_error("没有叫「"+bundle.collectionName+"」的合集可以覆盖");
                target=clash->id;
                for(const auto& row:r.saved){
                    if(!AutoRoute::SameRouteId(row.value("collection",std::string{AutoRoute::DefaultCollectionId}),target))continue;
                    const auto removed=row.value("id",std::string{});
                    try {DeleteRouteLocked(removed);}
                    catch(const AutoRoute::DeleteRollbackFailure&){if(r.active&&AutoRoute::SameRouteId(r.active->id,removed))StopNavigationLocked();ReloadSavedLocked();RefreshCompletedLocked();throw;}
                    catch(...){ReloadSavedLocked();RefreshCompletedLocked();throw;}
                    if(r.active&&AutoRoute::SameRouteId(r.active->id,removed)) {
                        InvalidateLocked();InvalidateAutoLocked(true);r.active.reset();r.runRequested=false;
                        r.completed.clear();r.skipHistory.clear();r.farmMode=false;r.farmConfirmation.Reset();
                    }
                    for(auto& [scene,draft]:r.drafts)if(draft.preview&&AutoRoute::SameRouteId(draft.preview->id,removed))draft.preview.reset();
                }
                // The list still describes the routes that were just deleted; the ids they held have
                // to be free again before the incoming ones are matched against them, or importing
                // the same bundle twice would keep inventing new ids.
                ReloadSavedLocked();
            }
            // Ids are file names, so "free" means no file holds it. A bundle that carries an id this
            // profile already uses gets a fresh one instead of quietly overwriting an unrelated
            // route — except in the replacing branch, where the collection was just emptied and the
            // ids it freed make importing the same bundle twice settle on the same answer.
            std::set<std::string> takenIds,takenNames;
            for(const auto& row:r.saved){
                takenIds.insert(LowerKey(row.value("id",std::string{})));
                if(AutoRoute::SameRouteId(row.value("collection",std::string{AutoRoute::DefaultCollectionId}),target))
                    takenNames.insert(LowerKey(row.value("name",std::string{})));
            }
            std::size_t written=0;
            for(auto plan:plans){
                if(!takenIds.insert(LowerKey(plan.id)).second){
                    auto replacement=NewId();
                    while(!takenIds.insert(LowerKey(replacement)).second)replacement=NewId();
                    plan.id=replacement;
                }
                // An imported copy must never inherit the source or a previously deleted route's local progress.
                for(auto& stop:plan.stops)if(AutoRoute::IsFreeStop(stop)){
                    const auto old=AutoRoute::Key(stop);stop.itemId="free:"+NewId();stop.freeRouteId=plan.id;
                    const auto next=AutoRoute::Key(stop);
                    if(plan.skipped.erase(old))plan.skipped.insert(next);
                    for(auto& key:plan.skipHistory)if(key==old)key=next;
                }
                plan.name=UniqueRouteNameLocked(plan.name,takenNames);
                takenNames.insert(LowerKey(plan.name));
                plan.collection=target;
                r.store->Save(plan,false);
                ++written;
            }
            // Entering what was just imported is the point for a collection: the next route the
            // player saves belongs beside the ones they came for.
            r.currentCollection=target;
            SaveCollectionsLocked();
            r.transfer=nullptr;
            ReloadSavedLocked();
            r.message="已导入 "+std::to_string(written)+" 条路线到「"+CollectionNameLocked(target)+"」"+
                (skipped? "，跳过 "+std::to_string(skipped)+" 条（点位资源已变化或文件损坏）":"");
            StructuredLogger::Record("info","routes","route-bundle-imported",
                "mode="+mode+" collectionId="+target+" routes="+std::to_string(written)+
                " skipped="+std::to_string(skipped)+" path="+AutoRoute::Utf8Text(path));
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
        }else if(action=="farm"){
            if(!r.active)throw std::runtime_error("请先开始一条自动路线");
            const bool enabled=command.value("enabled",!r.farmMode);
            if(r.farmMode!=enabled){
                // "刷怪采集" is a property of the route, so the toggle is written to the route
                // file before the runtime state moves: a failed save leaves both the file and
                // the switch as they were, instead of promising a memory that was not kept.
                auto next=*r.active;next.farmMode=enabled;
                r.store->Save(next,false);
                r.active=std::move(next);
                r.farmMode=enabled;r.farmConfirmation.Reset();r.farmNotice.clear();
            }
            r.message=enabled?"刷怪采集模式已开启：走到路线目标附近会自动标记采集物和敌人（已记在这条路线上）":
                "刷怪采集模式已关闭：路线目标仍需手动标记完成";
        }else if(action=="guide"){
            const int target=TargetIndexLocked();if(target<0||!r.active)throw std::runtime_error("当前没有可查看攻略的导航目标");
            auto item=r.active->stops[target];item.freeDisplayOrder=target+1;const auto profile=r.profile;const auto routeId=r.active->id;
            const auto scene=Scene::SceneIdToName(r.active->sceneId);POINT cursor{};GetCursorPos(&cursor);
            // This publishes the existing markerSelected event. It never
            // changes completion or advances the route.
            lock.unlock();DrawItemBase::SelectMarker(scene,item,cursor,profile);lock.lock();
            if(r.profile==profile&&r.active&&r.active->id==routeId)r.message="已打开当前目标攻略";
        }else if(action=="complete"){
            const int target=TargetIndexLocked();if(target<0||!r.active)throw std::runtime_error("当前没有待完成目标");
            auto item=r.active->stops[target];item.freeDisplayOrder=target+1;const auto profile=r.profile;const auto routeId=r.active->id;
            lock.unlock();const auto completed=AutoRoute::IsFreeStop(item)?CompleteFreePoint({{"profileId",profile},
                {"routeId",routeId},{"stateId",item.layer.stateId},{"pointId",item.itemId},{"completed",true}}):
                DrawItemBase::HandleMarkerCommand({{"type","markerSetCompletion"},{"profileId",profile},
                {"stateId",item.layer.stateId},{"pointId",item.itemId},{"completed",true}});
            if(!completed.value("accepted",false))throw std::runtime_error(completed.value("message","完成状态保存失败"));
            lock.lock();if(r.profile==profile&&r.active&&r.active->id==routeId)r.message="完成状态已保存";
        }else if(action=="save"){
            AutoRoute::Plan plan;const bool preview=command.value("target","")=="preview"||!r.active;
            if(preview){const auto& draft=DraftLocked();if(!draft.preview)throw std::runtime_error("没有可保存的预览");plan=*draft.preview;
                if(r.active&&r.active->id==plan.id)throw std::runtime_error("该预览已成为活动路线，请保存活动路线");
                // Only a route that has never been written takes the current collection. Saving an
                // already-saved active route rewrites its own file and must leave it where it is:
                // looking at another collection is not a request to move the player's work.
                plan.collection=NewRouteCollectionLocked(plan);}
            else plan=*r.active;
            plan.name=command.value("name",plan.name);r.store->Save(plan,!preview);
            if(preview)r.drafts[r.scene].preview=plan;else r.active=plan;ReloadSavedLocked();
            r.message=preview?"自动路线已保存到「"+CollectionNameLocked(plan.collection)+"」":"自动路线已保存";
        }else if(action=="load"){
            const auto next=r.store->Load(r.profile,command.at("routeId").get<std::string>(),ResolveLocked);ValidateTypedRoute(next);
            r.store->Save(next,true);InvalidateLocked();r.active=next;r.runRequested=false;r.skipHistory=next.skipHistory;r.enabled=false;r.tool="pan";
            r.farmMode=r.active->farmMode;r.farmConfirmation.Reset();r.farmNotice.clear();
            InvalidateAutoLocked(true);r.hasAutoPosition=false;
            for(auto& [scene,draft]:r.drafts)if(draft.preview&&draft.preview->id==next.id)draft.preview.reset();
            RefreshCompletedLocked();r.message="路线已加载，点击继续导航";
        }else if(action=="switch"){
            // Choosing a route out of the list means "follow this one now", which is what separates
            // it from `load`: the caller may still want the load-and-wait behaviour.
            auto next=r.store->Load(r.profile,command.at("routeId").get<std::string>(),ResolveLocked);ValidateTypedRoute(next);
            const bool start=command.value("start",true);
            r.store->Save(next,true);InvalidateLocked();
            r.active=next;r.runRequested=start;r.skipHistory=next.skipHistory;r.enabled=false;r.tool="pan";
            r.farmMode=r.active->farmMode;r.farmConfirmation.Reset();r.farmNotice.clear();
            InvalidateAutoLocked(true);r.hasAutoPosition=false;
            for(auto& [scene,draft]:r.drafts)if(draft.preview&&draft.preview->id==next.id)draft.preview.reset();
            RefreshCompletedLocked();
            r.message=start?"已应用该路线并开始指引":"已应用该路线，点击继续导航";
        }else if(action=="current"){
            // Reading only: the list page asks what the top row should say.
        }else if(action=="handStart"||action=="handPoint"||action=="handUndo"||action=="handCancel"||action=="handCommit"||action=="handFinish"||action=="handDiscard"||action=="handIcon"||action=="handChoose"){
            HandleHandDrawnLocked(command.value("action",std::string{}),command);
        }else if(action=="list"){ReloadSavedLocked();}
        else if(action!="state")throw std::invalid_argument("未知的自动路线操作");
        ++r.revision;result={{"accepted",true},{"message",r.message},{"data",SnapshotLocked()}};
    }catch(const std::exception& e){
        std::scoped_lock lock(r.mutex);r.message=e.what();++r.revision;
        // A refusal is the answer the player sees on the toolbar, and the only trace of it
        // afterwards: nothing else records that a command was turned down.
        StructuredLogger::Record("warn","routes","route-command-refused",
            "action="+command.value("action",std::string{"state"})+" reason="+std::string(e.what()));
        result={{"accepted",false},{"message",r.message},{"data",SnapshotLocked()}};
    }
    Emit();return result;
}
