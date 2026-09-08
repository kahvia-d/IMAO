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
    static std::string MarkerProfile(){return "local";}
    static bool IsPointCompleted(const std::string&,const ItemDatas& item){std::scoped_lock lock(mutex);return completed.contains(AutoRoute::Key(item));}
    static Json HandleMarkerCommand(const Json& command){
        const auto key=std::to_string(command.at("stateId").get<int>())+":"+command.at("pointId").get<std::string>();
        {std::scoped_lock lock(mutex);if(command.value("completed",false))completed.insert(key);else completed.erase(key);}
        RoutePlanningService::OnMarkerChanged();return {{"accepted",true}};
    }
    static void SelectMarker(const std::string&,const ItemDatas&,POINT,const std::string&){}
};
inline Coordinate RelativeCoordinates::IdentifyCoordToROC(const Coordinate& coordinate,int sceneId){
    const auto* scene=Scene::Find(sceneId);return scene?Coordinate(coordinate.x*scene->scale,-coordinate.y*scene->scale):Coordinate{};
}
