#pragma once
#include "Runtime/MarkerCompletionStore.h"
#include <algorithm>
#include <chrono>
#include <iostream>

namespace PublicationCompletionTests {
template<class Read>
bool Apply(const Read& read, ItemMarkerFrame& frame, MarkerCompletionStore& store, bool legacy) {
    if constexpr (requires { read.ApplyTo(frame); }) {
        if (!legacy) return read.ApplyTo(frame);
    }
    for (auto& item : frame.markers)
        item.isSaved = store.Completed(frame.sceneName, item.nameId, item.itemId);
    return true;
}

template<class Check>
void Test(const std::filesystem::path& root, Check check) {
    using Json = nlohmann::json;
    Json ids = Json::array();
    for (int i=0; i<12000; ++i) ids.push_back(std::to_string(1000000000000000000LL+i));
    const auto fixture=root/"publication-completions";
    WriteTextAtomically(fixture/"profiles/local.json", Json{{"schemaVersion",2},{"profileId","local"},
        {"revision",1},{"points",Json::object()},{"syncStates",Json::array({
            {{"stateId",8},{"initialized",true},{"remoteIds",ids}}})}}.dump());
    MarkerCompletionStore store(fixture);
    ItemMarkerFrame original; original.sceneName="World"; original.profileId="local";
    for(int i=0;i<250;++i) {
        ItemDatas point; point.nameId="collectible";
        point.itemId=std::to_string(1000000000000011875LL+i);
        original.markers.push_back(point);
    }
    const auto run=[&](bool legacy) {
        std::vector<double> timings;
        for(int n=0;n<5;++n) {
            auto frame=original;
            const auto started=std::chrono::steady_clock::now();
            PointRenderCompletionRead read; read.permanent=store.ReadRenderCompletions("World");
            read.farm.profileId=read.free.profileId="local";
            check(Apply(read,frame,store,legacy),"publication accepts a consistent account");
            timings.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count());
            for(int i=0;i<250;++i) check(frame.markers[i].isSaved==(i<125),"publication preserves cloud membership and missing IDs");
        }
        std::sort(timings.begin(),timings.end()); return timings[2];
    };
    const auto legacy=run(true), batch=run(false);
    std::cout<<"publication-completion legacyMs="<<legacy<<" batchMs="<<batch<<std::endl;
    check(batch*4<legacy,"publication must avoid a cloud-list scan per candidate (4x minimum)");
    PointRenderCompletionRead read; read.permanent=store.ReadRenderCompletions("World");
    read.farm.profileId=read.free.profileId="local";
    read.permanent.localById[original.markers[0].itemId]={"collectible",false};
    auto frame=original;
    check(Apply(read,frame,store,false)&&!frame.markers[0].isSaved,"publication honors local undo instead of stale source completion");
    auto daily=original.markers[0]; daily.nameId="daily"; daily.layer.stateId=912;
    auto freePoint=daily; freePoint.layer.stopKind=StopKind::Free; freePoint.freeRouteId="route";
    freePoint.freeCategory=FreePointCategory::Daily;
    read.dailyRouting=std::make_shared<const RenderDailyRouting>(RenderDailyRouting{{"daily"},{}});
    read.farm.identities.insert("912:"+daily.itemId);
    read.free.daily.insert(PointIdentityKey(freePoint));
    frame.markers={daily,freePoint};
    check(Apply(read,frame,store,false)&&frame.markers[0].isSaved&&frame.markers[1].isSaved,
        "publication routes daily state and free route identity to their own ledgers");
    read.farm.identities.clear(); read.free.daily.clear();
    check(Apply(read,frame,store,false)&&!frame.markers[0].isSaved&&!frame.markers[1].isSaved,
        "publication refresh observes daily reset without falling back to cloud completion");
    read.farm.profileId="other";
    check(!Apply(read,frame,store,false)&&frame.markers.empty(),"publication rejects mixed-account completion snapshots");
    read.farm.profileId="local"; frame=original; frame.profileId="other";
    check(!Apply(read,frame,store,false)&&frame.markers.empty(),"publication rejects a source frame belonging to another account");
}
}
