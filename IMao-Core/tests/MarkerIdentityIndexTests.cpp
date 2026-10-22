#define NOMINMAX
#include "Runtime/MarkerIdentityIndex.h"
#include <Windows.h>
#include <psapi.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <vector>
#pragma comment(lib, "psapi.lib")
using Json=nlohmann::json;
using LegacyIndex=std::unordered_map<std::string,Json>;
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void InsertLegacy(LegacyIndex& index,const std::string& scene,const std::string& category,int state,const std::string& id){
    index[std::to_string(state)+":"+id]={{"sceneName",scene},{"nameId",category},{"stateId",state},{"pointId",id}};
}
Json ExportLegacy(const LegacyIndex& index,int state){
    auto result=Json::array();
    for(const auto& [key,value]:index)if(state<=0||value.at("stateId")==state)result.push_back(value);
    return result;
}
void Parity(const MarkerIdentityIndex& index,const LegacyIndex& legacy){
    Require(index.Size()==legacy.size(),"identity count must match existing index");
    for(const auto& [key,identity]:legacy){
        Json actual={{"nameId","untrusted"},{"sceneName","wrong"},{"stateId",99},{"pointId","wrong"},{"completed",true},{"profileId","account"}};
        auto expected=actual;
        for(const auto& [name,value]:identity.items())expected[name]=value;
        Require(index.Apply(key,actual),"known identity must be found");
        Require(actual==expected,"normalization must preserve every identity and unrelated command field");
    }
    // Preserve the old unordered-map iteration order as well as membership.
    for(int state:{0,-1,8,77,900,902,903,905,906,909,910,912,999})Require(index.Export(state)==ExportLegacy(legacy,state),"sync state filtering and export order must match");
}
int main(int argc,char** argv){
    try{
        if(argc==5&&std::string(argv[1])=="--memory"){
            const bool verify=std::string(argv[2])=="verify";
            const bool compact=verify||std::string(argv[2])=="compact";
            Require(compact||std::string(argv[2])=="legacy","unknown memory probe mode");
            std::ofstream report{std::filesystem::path(argv[4])};Require(bool(report),"cannot open index memory report");
            const auto sample=[&](const char* point){
                PROCESS_MEMORY_COUNTERS_EX m{};
                Require(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m),sizeof(m))!=FALSE,"memory query failed");
                report<<Json{{"point",point},{"privateBytes",m.PrivateUsage},{"wsBytes",m.WorkingSetSize},{"peakWsBytes",m.PeakWorkingSetSize}}.dump()<<std::endl;
                Require(bool(report),"cannot persist index memory sample");
            };
            sample("before-json");
            const std::pair<const char*,int> scenes[]={{"World",8},{"Tethys",900},{"Fabricatorium",905},{"Avinoleum",903},{"Lahai",906},{"LowerVault",902},{"Darkplain",909},{"TimeRiftRuins",910},{"MengshuTianluo",912}};
            std::vector<Json> data;
            for(const auto& [scene,state]:scenes){
                std::ifstream input(std::filesystem::path(argv[3])/(std::string("itemsData_")+scene+".json"));
                Require(bool(input),"scene fixture missing");Json value;input>>value;data.push_back(std::move(value));
            }
            sample("json-ready");
            LegacyIndex legacy;MarkerIdentityIndex index;
            size_t identities=0;
            for(size_t i=0;i<data.size();++i)for(const auto& category:data[i]){
                const std::string scene=scenes[i].first,name=category.value("id","");
                const auto group=compact?index.AddGroup(scene,name):0;
                for(const auto& point:category.value("location",Json::array())){
                    if(!point.contains("id")||!point.at("id").is_string())continue;
                    const auto id=point.at("id").get<std::string>();const int state=point.value("stateId",scenes[i].second);
                    if(compact)index.Insert(group,state,id);
                    if(!compact||verify)InsertLegacy(legacy,scene,name,state,id);
                    ++identities;
                }
            }
            sample("index-ready");
            if(verify){Parity(index,legacy);std::cout<<"Full catalog parity passed\n";}
            std::cout<<"identities="<<(compact?index.Size():legacy.size())<<" inserted="<<identities<<'\n';
            return 0;
        }
        MarkerIdentityIndex index;LegacyIndex legacy;
        const auto a=index.AddGroup("World","permanent"),b=index.AddGroup("MengshuTianluo","daily");
        const auto add=[&](auto group,const char* scene,const char* category,int state,const std::string& id){index.Insert(group,state,id);InsertLegacy(legacy,scene,category,state,id);};
        add(a,"World","permanent",8,"same");add(b,"MengshuTianluo","daily",912,"same");
        add(a,"World","permanent",8,"has:colon");add(a,"World","permanent",0,"");
        add(a,"World","permanent",8,std::string("nul\0point:tail",14));
        add(a,"World","permanent",-1,"negative");add(b,"MengshuTianluo","daily",912,"中文点位");
        add(b,"MengshuTianluo","daily",8,"same"); // Existing duplicate key: last identity wins.
        Parity(index,legacy);
        Json unknown={{"nameId","unchanged"}};const auto before=unknown;
        Require(!index.Apply("8:missing",unknown)&&unknown==before,"unknown identity must not alter the command");
        std::string scene="temporary-scene",category="temporary-category",id="temporary-point-id-long-enough-for-owned-storage";
        const auto temporary=index.AddGroup(scene,category);index.Insert(temporary,77,id);
        InsertLegacy(legacy,scene,category,77,id);scene.clear();category.clear();id.clear();Parity(index,legacy);
        bool rejected=false;try{index.Insert(999999,8,"invalid");}catch(const std::out_of_range&){rejected=true;}
        Require(rejected,"invalid group must be rejected");Parity(index,legacy);
        const auto expectedSaved=ExportLegacy(legacy,0);
        const auto saved=index.Export();index.Clear();Require(index.Size()==0&&index.Export().empty(),"clear must reset identity and group state");
        Require(saved==expectedSaved,"exported JSON owns its contents after index clear");
        const auto group=index.AddGroup("World","reloaded");
        for(int i=0;i<24109;++i){
            const auto point="point-"+std::to_string(i);index.Insert(group,8,point);
        }
        Require(index.Size()==24109,"compact index keeps a full catalog");
        std::cout<<"Marker identity index tests passed\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
