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
inline void ValidateRouteComponent(const std::string& value) {
    if(value.empty()||value.size()>96||!std::all_of(value.begin(),value.end(),[](unsigned char c){
        return std::isalnum(c)||c=='-'||c=='_';}))throw std::invalid_argument("自动路线标识无效");
}
inline bool SameRouteId(const std::string& first,const std::string& second) {
    return first.size()==second.size()&&std::equal(first.begin(),first.end(),second.begin(),
        [](unsigned char a,unsigned char b){return std::tolower(a)==std::tolower(b);});
}
class DeleteRollbackFailure : public std::runtime_error {
public:
    DeleteRollbackFailure():std::runtime_error("删除自动路线失败且无法回滚，路线已暂停，请检查文件权限"){}
};

class RoutePlanStore {
public:
    using Json=nlohmann::json;
    using Resolver=std::function<std::optional<ItemDatas>(int,const std::string&)>;
    explicit RoutePlanStore(std::filesystem::path directory):root(std::move(directory)){}
    void Save(const Plan& plan,bool makeActive=false) const {
        Validate(plan);
        Json stops=Json::array();
        for(const auto& p:plan.stops) stops.push_back({{"stateId",p.layer.stateId},{"pointId",p.itemId},
            {"nameId",p.nameId},{"x",p.itemMapROC.x},{"y",p.itemMapROC.y},{"countryId",p.layer.countryId},
            {"floorId",p.layer.floorId},{"level",p.layer.level},{"skipped",plan.skipped.contains(Key(p))}});
        const Json doc={{"formatVersion",1},{"id",plan.id},{"name",plan.name},{"profileId",plan.profileId},
            {"sceneId",plan.sceneId},{"start",StartJson(plan.start)},{"stops",std::move(stops)},{"skipHistory",plan.skipHistory}};
        WriteTextAtomically(Path(plan.profileId,plan.id),doc.dump(2));
        if(makeActive)WriteTextAtomically(Folder(plan.profileId)/"active.json",
            Json({{"formatVersion",1},{"routeId",plan.id}}).dump(2));
    }
    Plan Load(const std::string& profile,const std::string& id,const Resolver& resolve) const {
        const auto doc=Read(Path(profile,id));
        if(doc.value("formatVersion",0)!=1||doc.value("profileId","")!=profile||doc.value("id","")!=id)
            throw std::runtime_error("自动路线文件版本或档案不匹配");
        Plan plan;plan.id=id;plan.profileId=profile;plan.name=doc.at("name").get<std::string>();
        plan.sceneId=doc.at("sceneId").get<int>();
        const auto& s=doc.at("start");plan.start={s.at("sceneId").get<int>(),
            Coordinate(s.at("x").get<double>(),s.at("y").get<double>()),s.at("source").get<std::string>(),
            s.value("confirmedUnixMs",std::int64_t{}),s.value("generation",std::uint64_t{}),s.at("valid").get<bool>()};
        if(!doc.at("stops").is_array()||doc.at("stops").empty()||doc.at("stops").size()>MaxTargets)
            throw std::runtime_error("自动路线目标数量无效");
        for(const auto& saved:doc.at("stops")) {
            const auto key=std::to_string(saved.at("stateId").get<int>())+":"+saved.at("pointId").get<std::string>();
            const auto found=resolve(plan.sceneId,key);
            if(!found||Key(*found)!=key)throw std::runtime_error("路线目标无法解析："+key);
            const double x=saved.at("x").get<double>(),y=saved.at("y").get<double>();
            if(!std::isfinite(x)||!std::isfinite(y)||saved.at("nameId")!=found->nameId||
                std::abs(x-found->itemMapROC.x)>1e-6||std::abs(y-found->itemMapROC.y)>1e-6)
                throw std::runtime_error("点位资源已变化，请重新规划："+key);
            plan.stops.push_back(*found);
            if(saved.value("skipped",false))plan.skipped.insert(key);
        }
        if(doc.contains("skipHistory"))plan.skipHistory=doc.at("skipHistory").get<std::vector<std::string>>();
        else for(const auto& item:plan.stops)if(plan.skipped.contains(Key(item)))plan.skipHistory.push_back(Key(item));
        Validate(plan);return plan;
    }
    std::optional<Plan> LoadActive(const std::string& profile,const Resolver& resolve) const {
        const auto path=Folder(profile)/"active.json";
        if(!std::filesystem::exists(path))return {};
        const auto doc=Read(path);
        if(doc.value("formatVersion",0)!=1)throw std::runtime_error("活动自动路线版本无效");
        if(doc.at("routeId").is_null())return {};
        const auto id=doc.at("routeId").get<std::string>();
        const auto routePath=Path(profile,id);
        // A process interruption after the deletion rename must not restore
        // a route which the user has explicitly removed.
        if(!std::filesystem::exists(routePath)&&std::filesystem::exists(DeletingPath(routePath)))return {};
        return Load(profile,id,resolve);
    }
    void ClearActive(const std::string& profile) const {
        WriteTextAtomically(Folder(profile)/"active.json",Json({{"formatVersion",1},{"routeId",nullptr}}).dump(2));
    }
    void Delete(const std::string& profile,const std::string& id) const {
        const auto routePath=Path(profile,id),pendingPath=DeletingPath(routePath);
        if(!std::filesystem::is_regular_file(routePath))throw std::runtime_error("要删除的自动路线不存在");
        bool wasActive=false;
        const auto activePath=Folder(profile)/"active.json";
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
    }
    Json List(const std::string& profile) const {
        Json rows=Json::array();const auto folder=Folder(profile);
        if(!std::filesystem::exists(folder))return rows;
        for(const auto& entry:std::filesystem::directory_iterator(folder)) {
            if(!entry.is_regular_file()||entry.path().extension()!=".json"||entry.path().filename()=="active.json")continue;
            const auto id=entry.path().stem().string();
            try {const auto doc=Read(entry.path());rows.push_back({{"id",id},{"name",doc.value("name",id)},
                {"sceneId",doc.value("sceneId",0)},{"sceneName",Scene::SceneIdToName(doc.value("sceneId",0))}});}
            catch(const std::exception&){rows.push_back({{"id",id},{"name",id+"（文件损坏）"},{"sceneId",0},{"sceneName",""}});}
        }
        std::sort(rows.begin(),rows.end(),[](const Json& a,const Json& b){return a.at("id")<b.at("id");});
        return rows;
    }
private:
    std::filesystem::path root;
    static std::filesystem::path DeletingPath(std::filesystem::path path) {path+=L".deleting";return path;}
    std::filesystem::path Folder(const std::string& profile) const {ValidateRouteComponent(profile);return root/std::filesystem::path(profile);}
    std::filesystem::path Path(const std::string& profile,const std::string& id) const {
        ValidateRouteComponent(id);
        auto normalized=id;std::transform(normalized.begin(),normalized.end(),normalized.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
        if(normalized=="active")throw std::invalid_argument("保留的自动路线标识");
        return Folder(profile)/std::filesystem::path(id+".json");
    }
    static Json Read(const std::filesystem::path& path) {
        if(!std::filesystem::exists(path)||std::filesystem::file_size(path)>2*1024*1024)
            throw std::runtime_error("无法读取自动路线文件");
        std::ifstream input(path);return Json::parse(input);
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
        for(const auto& skipped:p.skipped)if(!ids.contains(skipped))throw std::invalid_argument("跳过记录不属于路线");
        std::unordered_set<std::string> history;
        for(const auto& key:p.skipHistory)if(!p.skipped.contains(key)||!history.insert(key).second)
            throw std::invalid_argument("跳过撤销历史无效或重复");
    }
};
}
