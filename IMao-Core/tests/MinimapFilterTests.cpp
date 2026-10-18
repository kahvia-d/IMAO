#include "ImguiDraw/Items/DrawItemOnMinMap.h"
#include "Runtime/MarkerCompletionStore.h"
#include "Runtime/FarmCompletionStore.h"
#include "Runtime/RuntimeStatus.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {
std::shared_ptr<const std::vector<ItemsDatas>> selected;
std::unique_ptr<MarkerCompletionStore> permanent;
std::unique_ptr<FarmCompletionStore> farm;
std::int64_t day = 100;
void Check(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
ItemDatas Point(std::string id, std::string category, double x, double y = 0) {
    ItemDatas result; result.itemId = std::move(id); result.nameId = std::move(category);
    result.itemMapROC = {x,y}; result.layer.stateId = 8;
    return result;
}
void Set(const std::string& id, bool completed) {
    Check(permanent->Execute({{"type","markerSetCompletion"},{"sceneName","World"},
        {"stateId",8},{"nameId","collectible"},{"pointId",id},{"completed",completed}}).value("accepted",false), "fixture write failed");
}
}

// The fixture uses real, isolated on-disk ledgers. No production test hooks.
std::shared_ptr<const std::vector<ItemsDatas>> DrawItemBase::GetSceneItemsSnapshot(int) { return selected; }
std::string DrawItemBase::MarkerProfile() { return permanent->Profile(); }
std::uint64_t DrawItemBase::MarkerFilterRevision() { return 17; }
std::vector<std::string> DrawItemBase::GetFilteredPoints(std::string scene, std::string category) {
    return category == "daily" ? farm->CompletedIds(MarkerCompletionStore::SceneState(scene)) : permanent->CompletedIds(scene,category);
}
PointFilterCompletionRead DrawItemBase::ReadPointFilterCompletions(const std::string& scene) {
    PointFilterCompletionRead view;
    view.permanent = permanent->ReadCompletions(scene);
    view.farm = farm->ReadCompletions(MarkerCompletionStore::SceneState(scene));
    view.dailyCategories = {"daily"};
    return view;
}
void RuntimeStatus::SetMinimapMarkerCount(int) {}
namespace Diagnostics { void Record(const std::string&, const std::string&) {} }

int main() {
    const auto root = std::filesystem::temp_directory_path() / ("imao-mini-filter-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    try {
        permanent = std::make_unique<MarkerCompletionStore>(root / "permanent");
        farm = std::make_unique<FarmCompletionStore>(root / "farm", []{return day;});
        RECT rect{0,0,1920,1080}; Coordinate player{0,0};
        const auto update = [&] { DrawItemOnMinMap::UpdatePlayerNearItemsData(rect,player,1000,1); return DrawItemOnMinMap::Snapshot(); };
        // Reverting to per-category reads must fail even when almost all selected
        // points are far away: geometry culling does not excuse whole-ledger scans.
        nlohmann::json document = {{"schemaVersion",2},{"profileId","local"},{"revision",1},
            {"points",nlohmann::json::object()},{"syncStates",nlohmann::json::array()}};
        nlohmann::json remote = nlohmann::json::array();
        std::vector<ItemsDatas> groups;
        for (int c=0;c<24;++c) {
            ItemsDatas group; group.nameId = "category-"+std::to_string(c);
            for (int n=0;n<500;++n) {
                const auto id = std::to_string(c*500+n);
                group.itemsDatas.push_back(Point(id,group.nameId,n==0&&c==0?0:10000));
                remote.push_back(id);
                document["points"]["8:"+id]={{"sceneName","World"},{"stateId",8},{"nameId",group.nameId},{"pointId",id},{"completed",true}};
            }
            groups.push_back(std::move(group));
        }
        document["syncStates"].push_back({{"stateId",8},{"initialized",true},{"remoteIds",remote}});
        WriteTextAtomically(root / "permanent/profiles/local.json",document.dump());
        permanent = std::make_unique<MarkerCompletionStore>(root / "permanent");
        selected = std::make_shared<const std::vector<ItemsDatas>>(groups);
        std::vector<double> actual, legacy;
        for (int n=0;n<5;++n) {
            auto start=std::chrono::steady_clock::now();
            for (const auto& group:groups) Check(permanent->CompletedIds("World",group.nameId).size()==12000,"legacy fixture mismatch");
            legacy.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
            start=std::chrono::steady_clock::now(); const auto frame=update();
            actual.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
            Check(frame.markers.size()==1 && frame.markers.front().isSaved,"nearby filter lost completion or distant culling");
        }
        std::sort(actual.begin(),actual.end()); std::sort(legacy.begin(),legacy.end());
        std::cout<<"Minimap real filter legacyMs="<<legacy[2]<<" filterMs="<<actual[2]<<" ratio="<<actual[2]/legacy[2]<<std::endl;
        Check(actual[2]<legacy[2]*.25,"minimap filter must eliminate repeated whole-ledger scans (4x minimum)");

        permanent = std::make_unique<MarkerCompletionStore>(root / "small");
        DrawItemOnMinMap::ClearNearItemsData();
        selected=std::make_shared<const std::vector<ItemsDatas>>(std::vector<ItemsDatas>{
            {"collectible",{Point("near","collectible",0),Point("enter-edge","collectible",120),Point("outside","collectible",121)}},
            {"daily",{Point("daily-near","daily",0)}}});
        Set("near",true); farm->Set(8,"daily-near",true);
        auto frame=update(); Check(frame.markers.size()==3,"enter radius changed");
        Check(frame.markers[0].isSaved && frame.markers[2].isSaved,"permanent/daily completion routing changed");
        const auto center=ScreenCoordinate::MinMapCircleCenterScreenCoordinate(rect);
        Check(frame.markers[0].screenCoordiante.x==center.x && frame.markers[0].screenCoordiante.y==center.y,"center projection changed");
        Set("near",false); ++day;
        frame=update(); Check(!frame.markers[0].isSaved&&!frame.markers[2].isSaved,"undo/daily reset not immediate");
        player.x=-30; frame=update();
        Check(std::any_of(frame.markers.begin(),frame.markers.end(),[](const auto& p){return p.itemId=="enter-edge";}),"stay radius hysteresis lost");
        player.x=-36; frame=update();
        Check(std::none_of(frame.markers.begin(),frame.markers.end(),[](const auto& p){return p.itemId=="enter-edge";}),"stay radius no longer expires");
        Check(permanent->Execute({{"type","markerSelectProfile"},{"profileId","other"}}).value("accepted",false),"account fixture switch failed");
        Check(DrawItemOnMinMap::Snapshot().markers.empty(),"old minimap progress must not be relabeled as a new account");
        Check(update().markers.empty(),"mixed permanent/daily accounts must refuse a minimap marker set");
        farm->SelectProfile("other"); player={0,0}; frame=update();
        Check(frame.profileId=="other" && frame.markers.size()==3&&!frame.markers[0].isSaved,"next account inherited old progress");
        selected=std::make_shared<const std::vector<ItemsDatas>>();
        Check(update().markers.empty(),"empty selection retained old markers");
        selected=std::make_shared<const std::vector<ItemsDatas>>(std::vector<ItemsDatas>{
            {"collectible",{Point("center","collectible",0),Point("clipped","collectible",100)}}});
        DrawItemOnMinMap::UpdatePlayerNearItemsData(rect,player,1,1);
        frame=DrawItemOnMinMap::Snapshot();
        Check(frame.markers.size()==1&&frame.markers[0].itemId=="center","minimap circle clipping changed");
        permanent.reset(); farm.reset(); std::filesystem::remove_all(root);
        std::cout<<"Minimap filtering, completion, hysteresis and account tests passed"<<std::endl;
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<std::endl; return 1; }
}
