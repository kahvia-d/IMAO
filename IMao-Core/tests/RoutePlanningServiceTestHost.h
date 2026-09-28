#pragma once
// Included only when compiling the isolated service harness. No production IPC or
// executable exposes these observations or substitutes authoritative marker data.
#include "Runtime/RoutePlanningService.h"
#include "Coordinate/locationCalculator/RelativeCoordinates.h"
#include <filesystem>
#include <mutex>
#include <unordered_set>

struct StructuredLogger {
    inline static std::filesystem::path root;
    static std::filesystem::path ApplicationDataDirectory(){return root;}
    static void Record(const std::string&,const std::string&,const std::string&,const std::string&){}
};
struct DrawItemBase {
    using Json=nlohmann::json;
    inline static Json itemsJsonData_World=Json::array(),itemsJsonData_Tethys=Json::array(),itemsJsonData_Fabricatorium=Json::array(),
        itemsJsonData_Avinoleum=Json::array(),itemsJsonData_Lahai=Json::array(),itemsJsonData_LowerVault=Json::array(),
        itemsJsonData_Darkplain=Json::array(),itemsJsonData_TimeRiftRuins=Json::array();
    inline static std::mutex mutex;
    inline static std::unordered_set<std::string> completed;
    // Which identities the harness declares as daily-refresh (采集物 ∪ 敌人). The farming
    // tests fill these in; a test that leaves them empty proves the mode marks nothing.
    inline static std::unordered_set<std::string> refreshablePointIds;
    inline static std::unordered_set<std::string> refreshableCategories;
    static std::string MarkerProfile(){return "local";}
    static bool IsPointCompleted(const std::string&,const ItemDatas& item){std::scoped_lock lock(mutex);return completed.contains(AutoRoute::Key(item));}
    static bool IsRefreshablePoint(const std::string& nameId){return refreshableCategories.contains(nameId);}
    static bool IsRefreshablePointId(const std::string& pointId){return refreshablePointIds.contains(pointId);}
    // The isolated harness has no icon package. An empty path is also what the real one returns for
    // a type the manifest does not know, so the snapshot keeps the same shape either way.
    static std::string GetExternalIconPath(const std::string&){return {};}
    static bool ReportFarmLedgerExpiry(){return false;}
    static Json HandleMarkerCommand(const Json& command){
        std::size_t changed=0;
        {
            std::scoped_lock lock(mutex);
            if(command.value("type",std::string{})=="markerFarmComplete"){
                for(const auto& point:command.value("points",Json::array())){
                    const auto key=std::to_string(point.at("stateId").get<int>())+":"+point.at("pointId").get<std::string>();
                    if(completed.insert(key).second)++changed;
                }
            }else{
                const auto key=std::to_string(command.at("stateId").get<int>())+":"+command.at("pointId").get<std::string>();
                if(command.value("completed",false)){if(completed.insert(key).second)++changed;}
                else if(completed.erase(key))++changed;
            }
        }
        RoutePlanningService::OnMarkerChanged();
        return {{"accepted",true},{"message",""},{"data",{{"changed",changed}}}};
    }
    static void SelectMarker(const std::string&,const ItemDatas&,POINT,const std::string&){}
};
inline Coordinate RelativeCoordinates::IdentifyCoordToROC(const Coordinate& coordinate,int sceneId){
    const auto* scene=Scene::Find(sceneId);return scene?Coordinate(coordinate.x*scene->scale,-coordinate.y*scene->scale):Coordinate{};
}
