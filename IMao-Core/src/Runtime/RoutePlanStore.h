#pragma once
#include "RoutePlanningModel.h"
#include "AtomicFile.h"
#include <cctype>
#include <fstream>
#include <optional>
#include <nlohmann/json.hpp>

namespace AutoRoute {
inline nlohmann::json StartJson(const Start& s) {
    return {{"valid",s.valid},{"sceneId",s.sceneId},{"x",s.roc.x},{"y",s.roc.y},
        {"source",s.source},{"confirmedUnixMs",s.confirmedUnixMs},{"generation",s.generation}};
}
// The collection every route belongs to unless it was filed somewhere else. It is reserved: it is
// never stored in the collections index, it can never be created by hand, and it can never be
// renamed or deleted — which is what makes "the routes you already had are all in the default
// collection" true without any migration at all.
inline constexpr const char* DefaultCollectionId = "default";
// A single path-safe component. Route ids, profile ids and collection ids are all shaped like this
// so none of them can ever become a path surprise, and so the same validator serves all three.
inline bool IsRouteComponent(const std::string& value) {
    return !value.empty()&&value.size()<=96&&std::all_of(value.begin(),value.end(),[](unsigned char c){
        return std::isalnum(c)||c=='-'||c=='_';});
}
inline void ValidateRouteComponent(const std::string& value) {
    if(!IsRouteComponent(value))throw std::invalid_argument("自动路线标识无效");
}
// A collection id read from a file the player may have edited by hand. Anything unusable means
// "the default collection": the route stays visible and usable instead of disappearing because of
// a value nothing can name.
inline std::string NormalizeCollectionId(const std::string& value) {
    return IsRouteComponent(value)?value:std::string(DefaultCollectionId);
}
inline bool SameRouteId(const std::string& first,const std::string& second) {
    return first.size()==second.size()&&std::equal(first.begin(),first.end(),second.begin(),
        [](unsigned char a,unsigned char b){return std::tolower(a)==std::tolower(b);});
}
inline bool IsDefaultCollection(const std::string& value) {
    return SameRouteId(NormalizeCollectionId(value),DefaultCollectionId);
}
// Every store under SavedRoutes accepts either the root itself or one of its route folders, so a
// caller never has to care which form it happens to hold.
inline std::filesystem::path ResolveSavedRoutesRoot(std::filesystem::path directory) {
    const auto leaf=directory.filename().string();
    if(SameRouteId(leaf,"Auto")||SameRouteId(leaf,"Hand"))return directory.parent_path();
    return directory;
}
class DeleteRollbackFailure : public std::runtime_error {
public:
    DeleteRollbackFailure():std::runtime_error("删除自动路线失败且无法回滚，路线已暂停，请检查文件权限"){}
};

// Automatic and hand-drawn routes are two folders of the *same* store: same document schema,
// same validation, same delete/active handling. "Hand" only records where the route came from.
class RoutePlanStore {
public:
    using Json=nlohmann::json;
    using Resolver=std::function<std::optional<ItemDatas>(int,const std::string&)>;
    // `directory` may be the SavedRoutes root, or one of its route folders. A path whose last
    // component is already "Auto"/"Hand" is recognised, so both older callers and the service
    // can construct this without caring which form they hold.
    explicit RoutePlanStore(std::filesystem::path directory):root(ResolveRoot(std::move(directory))){}
    // The document a route is stored as. It is public because the export bundle has to hold exactly
    // this: one schema written by one function, so a route that survives being saved always survives
    // being carried to another machine.
    static Json Document(const Plan& plan) {
        Validate(plan);
        Json stops=Json::array();
        for(const auto& p:plan.stops) stops.push_back({{"stateId",p.layer.stateId},{"pointId",p.itemId},
            {"nameId",p.nameId},{"x",p.itemMapROC.x},{"y",p.itemMapROC.y},{"countryId",p.layer.countryId},
            {"floorId",p.layer.floorId},{"level",p.layer.level},{"skipped",plan.skipped.contains(Key(p))},
            // Written for every stop, including official ones: the reader must never have to
            // guess whether an empty nameId means "a new kind of stop" or "a corrupt file".
            {"kind",p.layer.stopKind==StopKind::Free?"free":"catalog"}});
        return {{"formatVersion",1},{"id",plan.id},{"name",plan.name},{"profileId",plan.profileId},
            {"sceneId",plan.sceneId},{"start",StartJson(plan.start)},{"stops",std::move(stops)},{"skipHistory",plan.skipHistory},
            // A missing field on an older file means "not a farming route", which is the only
            // safe reading: turning the mode on by default would auto-mark points on routes the
            // player never asked to farm.
            {"farmMode",plan.farmMode},
            {"handDrawn",plan.handDrawn},
            // Absent on older files means "on": that is what the player asked for when they
            // applied a route, and it is also the only reading that cannot silently change what
            // an existing route does.
            {"filterByRoute",plan.filterByRoute},
            // Absent on older files means the default collection, which is exactly where those
            // routes have always been shown. Written for every route so the reader never has to
            // guess, the same way `kind` is written for every stop.
            {"collection",NormalizeCollectionId(plan.collection)}};
    }
    void Save(const Plan& plan,bool makeActive=false) const {
        WriteTextAtomically(Path(plan.profileId,plan.id,plan.handDrawn),Document(plan).dump(2));
        if(makeActive)WriteTextAtomically(ActivePath(plan.profileId),
            Json({{"formatVersion",1},{"routeId",plan.id},{"handDrawn",plan.handDrawn}}).dump(2));
    }
    // An id identifies one route but not which folder holds it, so a plain load looks in both.
    // The hand-drawn folder is consulted first because it is the smaller one.
    Plan Load(const std::string& profile,const std::string& id,const Resolver& resolve) const {
        const auto handPath=Path(profile,id,true);
        if(std::filesystem::exists(handPath))return LoadFolder(profile,id,Root(profile,true),resolve);
        return LoadFolder(profile,id,Root(profile,false),resolve);
    }
    // The active pointer carries which folder the route lives in, so a hand-drawn route is
    // restorable without probing both folders (an id is unique, not shared).
    std::optional<Plan> LoadActive(const std::string& profile,const Resolver& resolve) const {
        const auto path=ActivePath(profile);
        if(!std::filesystem::exists(path))return {};
        const auto doc=Read(path);
        if(doc.value("formatVersion",0)!=1)throw std::runtime_error("活动自动路线版本无效");
        if(doc.at("routeId").is_null())return {};
        const auto id=doc.at("routeId").get<std::string>();
        const bool handDrawn=doc.value("handDrawn",false);
        const auto routePath=Path(profile,id,handDrawn);
        // A process interruption after the deletion rename must not restore
        // a route which the user has explicitly removed.
        if(!std::filesystem::exists(routePath)&&std::filesystem::exists(DeletingPath(routePath)))return {};
        return LoadFolder(profile,id,Root(profile,handDrawn),resolve);
    }
    void ClearActive(const std::string& profile) const {
        WriteTextAtomically(ActivePath(profile),Json({{"formatVersion",1},{"routeId",nullptr}}).dump(2));
    }
    // Deletes from whichever folder holds the id. Returns whether the route was hand-drawn, so
    // a caller can report which list the row disappeared from.
    bool Delete(const std::string& profile,const std::string& id) const {
        if(std::filesystem::is_regular_file(Path(profile,id,true)))return DeleteIn(profile,id,true);
        return DeleteIn(profile,id,false);
    }
    Json List(const std::string& profile) const {
        Json rows=Json::array();
        struct Row { std::string id,name,sceneName,collection=DefaultCollectionId; int sceneId=0,stopCount=0; bool handDrawn=false; std::vector<std::string> kinds; bool corrupt=false; };
        std::vector<Row> found;
        for(const bool handDrawn:{false,true}){
            const auto folder=Root(profile,handDrawn);
            if(!std::filesystem::exists(folder))continue;
            for(const auto& entry:std::filesystem::directory_iterator(folder)) {
                if(!entry.is_regular_file()||entry.path().extension()!=".json"||entry.path().filename()=="active.json")continue;
                Row row;row.id=entry.path().stem().string();row.handDrawn=handDrawn;
                try {
                    const auto doc=Read(entry.path());
                    row.name=doc.value("name",row.id);
                    row.sceneId=doc.value("sceneId",0);
                    row.sceneName=Scene::SceneIdToName(row.sceneId);
                    row.collection=NormalizeCollectionId(doc.value("collection",std::string{DefaultCollectionId}));
                    if(doc.contains("stops")&&doc.at("stops").is_array())for(const auto& stop:doc.at("stops")){
                        ++row.stopCount;
                        const auto nameId=stop.value("nameId",std::string{});
                        if(stop.value("kind",std::string{"catalog"})!="free"&&!nameId.empty()&&
                            std::find(row.kinds.begin(),row.kinds.end(),nameId)==row.kinds.end())row.kinds.push_back(nameId);
                    }
                } catch(const std::exception&){
                    row.name=row.id+"（文件损坏）";row.sceneId=0;row.sceneName.clear();row.corrupt=true;
                }
                found.push_back(std::move(row));
            }
        }
        std::sort(found.begin(),found.end(),[](const Row& a,const Row& b){return a.id<b.id;});
        for(auto& row:found){
            Json kinds=Json::array();for(const auto& kind:row.kinds)kinds.push_back({{"nameId",kind},{"name",kind}});
            rows.push_back({{"id",row.id},{"name",row.name},{"sceneId",row.sceneId},{"sceneName",row.sceneName},
                {"stopCount",row.stopCount},{"kinds",std::move(kinds)},{"handDrawn",row.handDrawn},
                {"corrupt",row.corrupt},{"collection",row.collection}});
        }
        return rows;
    }
private:
    std::filesystem::path root;
    static std::filesystem::path ResolveRoot(std::filesystem::path directory) {
        return ResolveSavedRoutesRoot(std::move(directory));
    }
    std::filesystem::path Root(const std::string& profile,bool handDrawn) const {
        ValidateRouteComponent(profile);
        return root/(handDrawn?"Hand":"Auto")/std::filesystem::path(profile);
    }
    // The pointer sits in the route folder it mostly governs and names the other folder when the
    // active route is a hand-drawn one, so loading never has to probe both.
    std::filesystem::path ActivePath(const std::string& profile) const {
        return Root(profile,false)/"active.json";
    }
    static std::filesystem::path DeletingPath(std::filesystem::path path) {path+=L".deleting";return path;}
    std::filesystem::path Path(const std::string& profile,const std::string& id,bool handDrawn) const {
        ValidateRouteComponent(id);
        auto normalized=id;std::transform(normalized.begin(),normalized.end(),normalized.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
        if(normalized=="active")throw std::invalid_argument("保留的自动路线标识");
        return Root(profile,handDrawn)/std::filesystem::path(id+".json");
    }
    static Json Read(const std::filesystem::path& path) {
        if(!std::filesystem::exists(path)||std::filesystem::file_size(path)>2*1024*1024)
            throw std::runtime_error("无法读取自动路线文件");
        std::ifstream input(path);return Json::parse(input);
    }
    Plan LoadFolder(const std::string& profile,const std::string& id,const std::filesystem::path& folder,const Resolver& resolve) const {
        const auto doc=Read(folder/(id+".json"));
        if(doc.value("formatVersion",0)!=1||doc.value("profileId","")!=profile||doc.value("id","")!=id)
            throw std::runtime_error("自动路线文件版本或档案不匹配");
        Plan plan;plan.id=id;plan.profileId=profile;plan.name=doc.at("name").get<std::string>();
        plan.sceneId=doc.at("sceneId").get<int>();
        const auto& s=doc.at("start");plan.start={s.at("sceneId").get<int>(),
            Coordinate(s.at("x").get<double>(),s.at("y").get<double>()),s.at("source").get<std::string>(),
            s.value("confirmedUnixMs",std::int64_t{}),s.value("generation",std::uint64_t{}),s.at("valid").get<bool>()};
        if(!doc.at("stops").is_array()||doc.at("stops").empty()||doc.at("stops").size()>MaxTargets)
            throw std::runtime_error("自动路线目标数量无效");
        const auto* scene=Scene::Find(plan.sceneId);
        if(!scene)throw std::runtime_error("自动路线场景无效");
        for(const auto& saved:doc.at("stops")) {
            const auto pointId=saved.at("pointId").get<std::string>();
            const auto stateId=saved.at("stateId").get<int>();
            const double x=saved.at("x").get<double>(),y=saved.at("y").get<double>();
            if(!std::isfinite(x)||!std::isfinite(y))throw std::runtime_error("自动路线坐标无效");
            if(stateId!=scene->kuroStateId)throw std::runtime_error("自动路线目标不属于该地图");
            const auto key=std::to_string(stateId)+":"+pointId;
            if(saved.value("kind",std::string{"catalog"})=="free"){
                // A free point was invented by the player on empty map space. There is nothing in
                // the catalogue to resolve it against, so it is trusted as stored — which is
                // exactly why only free points are allowed to be missing a type.
                ItemDatas free;free.itemId=pointId;free.itemMapROC=Coordinate(x,y);
                free.layer.stateId=stateId;free.layer.countryId=saved.value("countryId",0);
                free.layer.floorId=saved.value("floorId",std::string{});
                free.layer.level=saved.value("level",std::string{});
                free.layer.stopKind=StopKind::Free;
                plan.stops.push_back(std::move(free));
            }else{
                const auto found=resolve(plan.sceneId,key);
                if(!found||Key(*found)!=key)throw std::runtime_error("路线目标无法解析："+key);
                if(saved.at("nameId")!=found->nameId||
                    std::abs(x-found->itemMapROC.x)>1e-6||std::abs(y-found->itemMapROC.y)>1e-6)
                    throw std::runtime_error("点位资源已变化，请重新规划："+key);
                plan.stops.push_back(*found);
            }
            if(saved.value("skipped",false))plan.skipped.insert(key);
        }
        if(doc.contains("skipHistory"))plan.skipHistory=doc.at("skipHistory").get<std::vector<std::string>>();
        else for(const auto& item:plan.stops)if(plan.skipped.contains(Key(item)))plan.skipHistory.push_back(Key(item));
        plan.farmMode=doc.value("farmMode",false);
        plan.handDrawn=doc.value("handDrawn",false);
        plan.filterByRoute=doc.value("filterByRoute",true);
        plan.collection=NormalizeCollectionId(doc.value("collection",std::string{DefaultCollectionId}));
        Validate(plan);return plan;
    }
    bool DeleteIn(const std::string& profile,const std::string& id,bool handDrawn) const {
        const auto routePath=Path(profile,id,handDrawn),pendingPath=DeletingPath(routePath);
        if(!std::filesystem::is_regular_file(routePath))throw std::runtime_error("要删除的自动路线不存在");
        bool wasActive=false;
        const auto activePath=ActivePath(profile);
        if(std::filesystem::exists(activePath)){
            // A damaged active pointer must not prevent removing a damaged
            // saved route. Normal pointers still receive a durable clear.
            try {const auto doc=Read(activePath);wasActive=doc.contains("routeId")&&doc.at("routeId").is_string()&&
                SameRouteId(doc.at("routeId").get<std::string>(),id);}
            catch(const Json::exception&){}
        }
        if(!MoveFileExW(routePath.c_str(),pendingPath.c_str(),MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("无法删除自动路线：Windows error "+std::to_string(GetLastError()));
        try {if(wasActive)ClearActive(profile);}
        catch(...){
            if(!MoveFileExW(pendingPath.c_str(),routePath.c_str(),MOVEFILE_WRITE_THROUGH))
                throw DeleteRollbackFailure();
            throw;
        }
        // The rename is the deletion commit. A leftover tombstone is excluded
        // from the saved list and recognized by LoadActive after a crash.
        std::error_code cleanupError;
        std::filesystem::remove(pendingPath,cleanupError);
        return handDrawn;
    }
    static void Validate(const Plan& p) {
        ValidateRouteComponent(p.profileId);ValidateRouteComponent(p.id);
        if(p.name.empty()||p.name.size()>256||!Scene::IsKnown(p.sceneId)||!p.start.valid||p.start.sceneId!=p.sceneId||
            !std::isfinite(p.start.roc.x)||!std::isfinite(p.start.roc.y)||p.stops.empty()||p.stops.size()>MaxTargets)
            throw std::invalid_argument("自动路线内容无效");
        const auto* scene=Scene::Find(p.sceneId);std::unordered_set<std::string> ids;
        for(const auto& item:p.stops)if(item.itemId.empty()||item.layer.stateId!=scene->kuroStateId||
            !std::isfinite(item.itemMapROC.x)||!std::isfinite(item.itemMapROC.y)||!ids.insert(Key(item)).second)
            throw std::invalid_argument("自动路线目标无效或重复");
        for(const auto& item:p.stops)if(IsFreeStop(item)&&!item.nameId.empty())
            throw std::invalid_argument("自由点不能带有点位类型");
        for(const auto& skipped:p.skipped)if(!ids.contains(skipped))throw std::invalid_argument("跳过记录不属于路线");
        std::unordered_set<std::string> history;
        for(const auto& key:p.skipHistory)if(!p.skipped.contains(key)||!history.insert(key).second)
            throw std::invalid_argument("跳过撤销历史无效或重复");
    }
};
}
