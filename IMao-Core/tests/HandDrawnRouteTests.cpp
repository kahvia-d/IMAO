// Tests for drawing a route by hand: the ordered draft the picker collects, and the plan it
// commits. These are pure logic and need no map data, no marker store and no IPC.
#include "Runtime/HandDrawnRoute.h"
#include "Runtime/HandRouteInput.h"
#include "Runtime/RoutePlanStore.h"
#include "Runtime/RouteCollections.h"
#include "Runtime/LegacyHandRouteImport.h"
#include "Runtime/FreePointCompletionStore.h"
#include "Runtime/RouteGeometry.h"
#include "ImguiDraw/Routes/DrawFreePointBadge.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace {
int failures = 0;
void Check(bool condition, const std::string& message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
bool Near(Coordinate a, Coordinate b) { return std::abs(a.x - b.x) <= 1e-9 && std::abs(a.y - b.y) <= 1e-9; }

constexpr int Scene1 = 1;                 // runtime scene: what a route stores
int StateIdFor(int scene) { return Scene::Find(scene)->kuroStateId; }

// An official point on the map, as the picker would hand it over when the player clicks an icon.
ItemDatas Official(const std::string& id, double x, double y, int state) {
    ItemDatas item;
    item.itemId = id; item.nameId = "chest"; item.itemMapROC = {x, y};
    item.layer.stateId = state;
    return item;
}
// Empty map space: the picker knows only where the cursor was.
ItemDatas Blank(double x, double y, int state) {
    ItemDatas item;
    item.itemMapROC = {x, y};
    item.layer.stateId = state;
    return item;
}
void Rejects(const std::function<void()>& action, const std::string& message) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    Check(rejected, message);
}

void ConnectAll(AutoRoute::HandDrawnDraft& draft) {
    const auto nodes=draft.Editor().nodes;
    for(std::size_t i=1;i<nodes.size();++i)draft.Connect(nodes[i-1].id,nodes[i].id);
}

void DraftTests() {
    AutoRoute::HandDrawnDraft draft;
    Check(!draft.Active() && draft.Size() == 0, "a draft starts inactive and empty");
    Rejects([&] { draft.Add(Blank(0, 0, StateIdFor(Scene1))); }, "adding a point before starting is refused");
    Rejects([&] { draft.Start(999, StateIdFor(Scene1)); }, "drawing on an unknown map is refused");
    Check(!draft.Active(), "a refused start leaves the draft inactive");

    draft.Start(Scene1, StateIdFor(Scene1));
    Check(draft.Active() && draft.SceneId() == Scene1, "starting records the map the drawing belongs to");
    const auto first = draft.Add(Blank(10, 20, StateIdFor(Scene1)));
    Check(first.itemId == "free:1" && AutoRoute::IsFreeStop(first) && first.nameId.empty() &&
        Near(first.itemMapROC, {10, 20}),
        "a click on empty space becomes the first numbered free point with no type");
    const auto second = draft.Add(Official("chest-a", 30, 40, StateIdFor(Scene1)));
    Check(second.itemId == "chest-a" && !AutoRoute::IsFreeStop(second) && second.nameId == "chest",
        "a click on an official point connects to that point and keeps its type");
    const auto third = draft.Add(Blank(50, 60, StateIdFor(Scene1)));
    Check(third.itemId == "free:2", "free points are numbered in the order they were drawn");
    Check(draft.Size() == 3, "every click adds exactly one stop");

    Rejects([&] { draft.Add(Official("other", 0, 0, StateIdFor(2))); },
        "a point from another map is refused instead of making a route span two maps");
    Check(draft.Size() == 3, "a refused point does not enter the draft");

    Check(draft.Undo() && draft.Size() == 2, "undo removes the last stop");
    const auto afterUndo = draft.Add(Blank(70, 80, StateIdFor(Scene1)));
    Check(afterUndo.itemId == "free:3",
        "undo must not reuse a deleted point's completion identity; visible order is separate");
    Check(draft.Undo() && draft.Undo() && draft.Undo() && !draft.Undo() && draft.Size() == 0,
        "undo empties the draft and then reports there is nothing left");

    // The first stop is the start and the last is the end: the player drew an ordered path, so
    // nothing here may reorder it.
    draft.Cancel();
    draft.Start(Scene1, StateIdFor(Scene1));
    draft.Add(Blank(1, 1, StateIdFor(Scene1)));
    draft.Add(Official("chest-a", 30, 40, StateIdFor(Scene1)));
    draft.Add(Blank(9, 9, StateIdFor(Scene1)));
    ConnectAll(draft);
    const auto plan = draft.Commit("route-1", "我的路线", "local");
    Check(plan.handDrawn && plan.sceneId == Scene1 && plan.id == "route-1" && plan.name == "我的路线" &&
        plan.profileId == "local", "the committed plan carries the route's identity and is marked hand-drawn");
    Check(plan.start.valid && plan.start.sceneId == Scene1 && Near(plan.start.roc, {1, 1}),
        "the first stop is the start of the route");
    Check(plan.stops.size() == 3 && plan.stops.front().itemId == "free:1" && plan.stops.back().itemId == "free:2" &&
        plan.stops[1].itemId == "chest-a",
        "the committed stops keep the order the player drew, with the last one as the end");
    Check(plan.start.source == "manual", "a hand-drawn start is recorded as manual, not as a player snapshot");

    // One point is not a route: there would be no line to draw and no end.
    draft.Cancel();
    draft.Start(Scene1, StateIdFor(Scene1));
    draft.Add(Blank(1, 1, StateIdFor(Scene1)));
    Rejects([&] { draft.Commit("route-2", "x", "local"); }, "a drawing with a single point cannot be committed");

    // Leaving the drawing keeps it. The player cannot reach the toolbar while the drawing still owns
    // the map, so discarding on the way out would destroy work they never had a chance to save.
    draft.Cancel();
    draft.Start(Scene1, StateIdFor(Scene1));
    draft.Add(Blank(1, 1, StateIdFor(Scene1)));
    draft.Add(Blank(2, 2, StateIdFor(Scene1)));
    Check(draft.Finish() && !draft.Active() && draft.Pending() && draft.Size() == 2,
        "finishing a drawing stops it without losing what was drawn");
    draft.Start(Scene1,StateIdFor(Scene1));ConnectAll(draft);draft.Finish();
    const auto kept = draft.Commit("route-kept", "我的路线", "local");
    Check(kept.handDrawn && kept.stops.size() == 2 && kept.sceneId == Scene1,
        "a finished drawing can still be committed afterwards");
    Check(draft.Pending(), "committing does not itself clear the drawing; the caller decides");
    draft.Cancel();
    Check(!draft.Active() && !draft.Pending() && draft.Size() == 0,
        "cancelling clears a finished drawing too");
    Check(!draft.Finish(), "finishing when nothing is being drawn reports that there was nothing to finish");
    draft.Start(Scene1, StateIdFor(Scene1));
    Check(!draft.Finish() && !draft.Pending() && draft.Size() == 0,
        "finishing an empty drawing leaves nothing pending");

    draft.Start(Scene1, StateIdFor(Scene1));
    draft.Add(Blank(1, 1, StateIdFor(Scene1)));
    draft.Add(Blank(2, 2, StateIdFor(Scene1)));
    draft.Finish();
    Check(draft.Undo() && draft.Size() == 1 && draft.Pending(),
        "a finished drawing can still be undone before it is saved");
    draft.Cancel();
    Rejects([&] { draft.Commit("route-3", "x", "local"); }, "committing after cancelling is refused");

    // Coming back to a finished drawing continues it. This is the one place where "start" could have
    // destroyed the player's work, which is why it resumes instead of clearing.
    draft.Start(Scene1, StateIdFor(Scene1));
    draft.Add(Blank(1, 1, StateIdFor(Scene1)));
    draft.Add(Blank(2, 2, StateIdFor(Scene1)));
    draft.Finish();
    draft.Start(Scene1, StateIdFor(Scene1));
    Check(draft.Active() && !draft.Pending() && draft.Size() == 2,
        "starting again on the same map resumes the finished drawing instead of clearing it");
    const auto resumed = draft.Add(Blank(3, 3, StateIdFor(Scene1)));
    Check(resumed.itemId == "free:3" && draft.Size() == 3,
        "a resumed drawing keeps numbering where it left off");
    Check(draft.Finish() && draft.Pending() && draft.Size() == 3,
        "a resumed drawing can be finished again holding everything it had");
    Rejects([&] { draft.Start(2, StateIdFor(2)); },
        "a finished drawing belonging to another map cannot be silently taken over");

    // After discarding, starting again must still produce an empty drawing.
    draft.Cancel();
    draft.Start(Scene1, StateIdFor(Scene1));
    Check(draft.Active() && draft.Size() == 0, "after discarding, starting again begins an empty drawing");
    draft.Cancel();
}

// Catches implicit click-order navigation, non-atomic rewires and lost orphan nodes.
void EditorTests() {
    AutoRoute::HandDrawnDraft draft;
    draft.Start(Scene1, StateIdFor(Scene1), FreePointCategory::Daily, "editor");
    for(int i=0;i<4;++i) draft.Add(Blank(i*10,0,StateIdFor(Scene1)));
    Check(draft.OrderedPoints().empty()&&!draft.CanCommit(),"clicks create nodes without a route");
    draft.Connect("n1","n2");draft.Connect("n2","n3");draft.Connect("n3","n4");
    Check(draft.CanCommit()&&draft.OrderedPoints().size()==4,"dragged edges form one ordered chain");
    draft.Connect("n1","n3");
    Check(draft.Editor().edges==std::vector<AutoRoute::HandRouteEdge>{{"n3","n4"},{"n1","n3"}},"rewiring removes both old A-B and B-C edges");
    auto plan=draft.Commit("editor","Editor","local");
    Check(plan.stops.size()==3&&plan.stops[0].itemId=="free:1"&&plan.stops[1].itemId=="free:3"&&plan.stops[2].itemId=="free:4","rewiring leaves B out of A-C-D navigation");
    Check(plan.handEditor&&plan.handEditor->nodes.size()==4,"orphan B remains in the saved editor");
    Rejects([&]{draft.Connect("n4","n1");},"a cycle is rejected atomically");
    Check(draft.OrderedPoints().size()==3,"a rejected cycle preserves the chain");
    draft.Remove("n3");
    Check(draft.OrderedPoints().size()==2&&draft.OrderedPoints().back().itemId=="free:4","deleting the middle point reconnects its neighbours");
    draft.Undo();draft.Undo();
    Check(draft.OrderedPoints().size()==4,"undo restores removal and the old connections");
    draft.Connect("n2","n1");
    Check(!draft.CanCommit(),"reversing one edge can leave two fragments, which cannot be saved");
    draft.Connect("n1","n3");
    Check(draft.CanCommit()&&draft.OrderedPoints().front().itemId=="free:2","joining fragments gives an unambiguous new start");
    plan=draft.Commit("editor","Editor","local");
    AutoRoute::HandDrawnDraft edit;edit.Edit(plan);
    Check(edit.Active()&&edit.RouteId()=="editor"&&edit.OrderedPoints().front().itemId=="free:2","saved routes enter the same editor without changing identity");
    const auto document=AutoRoute::RoutePlanStore::Document(plan);
    const auto parsed=AutoRoute::RoutePlanStore::Parse(document,"local","editor",[](int,const std::string&)->std::optional<ItemDatas>{return {};});
    Check(document.at("formatVersion")==3&&parsed.handEditor&&parsed.handEditor->nodes.size()==4,"v3 preserves every editor node and the path across reload");
    auto copied=parsed;copied.id="editor-copy";AutoRoute::ScopeFreePoints(copied,copied.id);
    Check(copied.handEditor->nodes.front().point.freeRouteId=="editor-copy","import copies scope editor-only free points to the new route");
    auto legacy=plan;legacy.handEditor.reset();legacy.stops[0].itemId="free:ab12-opaque-uuid";
    AutoRoute::HandDrawnDraft legacyEdit;legacyEdit.Edit(legacy);
    const auto upgraded=legacyEdit.Commit(legacy.id,legacy.name,"local");
    Check(AutoRoute::RoutePlanStore::Document(upgraded).at("formatVersion")==3,"legacy opaque free IDs remain editable and savable");
    AutoRoute::HandDrawnDraft bounded;bounded.Start(Scene1,StateIdFor(Scene1));
    for(int i=0;i<101;++i) bounded.Add(Blank(i,0,StateIdFor(Scene1)));
    int undos=0;while(bounded.Undo())++undos;
    Check(undos==100&&bounded.Size()==1,"editor history is capped at the last 100 operations");
}

void EditorInputTests() {
    using Clock=std::chrono::steady_clock;const auto now=Clock::now();
    AutoRoute::HandDeleteConfirmation deletion;
    Check(!deletion.Down("n1",7,now),"first Backspace arms without removing a point");
    Check(!deletion.Down("n1",7,now+std::chrono::milliseconds(10)),"key repeat cannot confirm deletion");
    deletion.Up();
    Check(deletion.Down("n1",7,now+std::chrono::milliseconds(20)),"a separate second press confirms the same point");
    deletion.Up();deletion.Down("n1",7,now);deletion.Up();
    deletion.Observe("n2",7,now+std::chrono::milliseconds(10),true);
    Check(!deletion.Down("n1",7,now+std::chrono::milliseconds(20)),"leaving a point clears its deletion confirmation");
    deletion.Up();deletion.Reset();deletion.Down("n1",7,now);deletion.Up();
    Check(!deletion.Down("n1",7,now+std::chrono::seconds(3)),"expired confirmation needs two new presses");
    deletion.Up();deletion.Reset();deletion.Down("n1",7,now);deletion.Up();
    Check(!deletion.Down("n1",8,now+std::chrono::milliseconds(20)),"another edit invalidates pending deletion");
    deletion.Up();deletion.Reset();deletion.Down("n1",7,now);deletion.Up();deletion.Observe("n1",7,now,false);
    Check(!deletion.Down("n1",7,now+std::chrono::milliseconds(20)),"focus loss disarms deletion");
    AutoRoute::HandPointerGesture pointer;
    pointer.Begin("n1",{10,10},4);pointer.Move({12,10});
    Check(pointer.End("n2")==AutoRoute::HandPointerAction::Click,"sub-threshold movement remains a click");
    pointer.Begin("n1",{10,10},4);pointer.Move({20,10});
    Check(pointer.End("n2")==AutoRoute::HandPointerAction::Connect,"dragging to another point connects");
    pointer.Begin("n1",{10,10},4);pointer.Move({20,10});
    Check(pointer.End("")==AutoRoute::HandPointerAction::Cancel,"dragging onto empty ground never adds a point");
    pointer.Begin("",{10,10},4);pointer.Move({20,10});
    Check(pointer.End("n2")==AutoRoute::HandPointerAction::Cancel,"dragging empty ground never creates a point");
}

void ImportTests() {
    using Json = nlohmann::json;
    const auto lookupScene = [](const std::string& name) { return Scene::SceneNameToId(name.c_str()); };
    const auto noPoints = [](int, const Coordinate&) -> std::optional<ItemDatas> { return {}; };
    const auto document = Json::parse(R"({"World":[[[0,0],[10,0]],[[10,0],[20,0]]]})");
    const auto imported = AutoRoute::LegacyDocument::Import(document, "Routes", "local", lookupScene, noPoints);
    Check(imported.rejected.empty() && imported.plans.size() == 1, "a legacy file becomes one hand-drawn route");
    const auto& plan = imported.plans.front();
    Check(plan.handDrawn && plan.sceneId == 1 && plan.stops.size() == 4 &&
        plan.stops.front().itemId == "free:1" && plan.stops.back().itemId == "free:4",
        "each endpoint of each segment becomes its own numbered stop in drawing order");
    Check(plan.start.valid && Near(plan.start.roc, {0, 0}), "an imported route starts at its first point");
    Check(std::all_of(plan.stops.begin(), plan.stops.end(),
            [](const ItemDatas& stop) { return AutoRoute::IsFreeStop(stop) && stop.nameId.empty(); }),
        "an import invents positions but never invents a point type");

    const auto chest = Official("chest", 10, 0, StateIdFor(1));
    const auto withPoint = AutoRoute::LegacyDocument::Import(document, "Routes", "local", lookupScene,
        [&](int scene, const Coordinate&) -> std::optional<ItemDatas> { return scene == 1 ? std::optional<ItemDatas>{chest} : std::nullopt; });
    Check(!AutoRoute::IsFreeStop(withPoint.plans.front().stops[1]) &&
        withPoint.plans.front().stops[1].itemId == "chest" && AutoRoute::IsFreeStop(withPoint.plans.front().stops[0]),
        "only the endpoint sitting on an official point is upgraded; the rest stay free");

    const auto twoScenes = Json::parse(R"({"World":[[[0,0],[1,1]]],"Tethys":[[[2,2],[3,3]]]})");
    const auto split = AutoRoute::LegacyDocument::Import(twoScenes, "Routes", "local", lookupScene, noPoints);
    Check(split.plans.size() == 2 && split.plans[0].sceneId != split.plans[1].sceneId &&
        split.plans[0].stops.size() == 2 && split.plans[1].stops.size() == 2,
        "a legacy file spanning two maps imports as one route per map");

    for (const auto& rejected : {Json::parse(R"({"NoSuchPlace":[[[0,0],[1,1]]]})"),
            Json::parse(R"({"World":[[[0,0]]]})"), Json::parse(R"([1,2,3])")})
        Check(!AutoRoute::LegacyDocument::Import(rejected, "x", "local", lookupScene, noPoints).rejected.empty(),
            "a legacy file that cannot be understood is refused rather than guessed at");
}
void FreeIdentityTests() {
    AutoRoute::HandDrawnDraft a, b;
    a.Start(Scene1, StateIdFor(Scene1)); b.Start(Scene1, StateIdFor(Scene1));
    for (auto* draft : {&a, &b}) {
        draft->Add(Blank(1, 2, StateIdFor(Scene1)));
        draft->Add(Blank(3, 4, StateIdFor(Scene1)));
    }
    ConnectAll(a);ConnectAll(b);
    const auto first = a.Commit("route-a", "A", "local");
    const auto second = b.Commit("route-b", "B", "local");
    Check(AutoRoute::Key(first.stops[0]) != AutoRoute::Key(second.stops[0]),
        "two routes' first free stops must have different completion identities");
}
void FreeIconAndMigrationTests() {
    AutoRoute::HandDrawnDraft draft;draft.Start(Scene1,StateIdFor(Scene1),FreePointCategory::Daily,"icons");
    std::vector<FreePointIcon> icons={FreePointIcon::Number,FreePointIcon::Monster,FreePointIcon::Monster1C,
        FreePointIcon::Monster3C,FreePointIcon::Plant,FreePointIcon::Ore};
    for(auto icon:icons){draft.SelectIcon(icon);draft.Add(Blank(10+draft.Size(),20,StateIdFor(Scene1)));}
    ConnectAll(draft);
    auto plan=draft.Commit("icons","icons","local");
    plan.skipped.insert(AutoRoute::Key(plan.stops.front()));plan.skipHistory.push_back(AutoRoute::Key(plan.stops.front()));
    auto document=AutoRoute::RoutePlanStore::Document(plan);
    const auto none=[](int,const std::string&)->std::optional<ItemDatas>{return {};};
    const auto parsed=AutoRoute::RoutePlanStore::Parse(document,"local","icons",none);
    Check(parsed.skipped==plan.skipped&&parsed.skipHistory==plan.skipHistory,"v2 skipped free points reload with their scoped identity and history");
    for(std::size_t i=0;i<icons.size();++i)Check(parsed.stops[i].freeIcon==icons[i],"switching icons affects only subsequent points and all six survive a v2 round trip");
    const auto firstKey=AutoRoute::Key(plan.stops.front());draft.Remove(draft.Editor().nodes.back().id);draft.SelectIcon(FreePointIcon::Ore);draft.Add(Blank(50,60,StateIdFor(Scene1)));ConnectAll(draft);
    Check(AutoRoute::Key(draft.Commit("icons","icons","local").stops.front())==firstKey,"undo and new drawing never change surviving identities");
    document["formatVersion"]=1;document.erase("handEditor");document.erase("routeCategory");document.erase("legacyHandDrawn");
    document["stops"][0]["skipped"]=true;
    document["skipHistory"]=nlohmann::json::array({std::to_string(StateIdFor(Scene1))+":free:1"});
    const auto legacy=AutoRoute::RoutePlanStore::Parse(document,"local","icons",none);
    Check(legacy.legacyHandDrawn&&legacy.skipHistory.front()==firstKey&&legacy.skipped.contains(firstKey),"v1 migration rewrites skipped identity and its undo history deterministically");
    Check(legacy.stops[3].freeIcon==FreePointIcon::Number&&legacy.stops[3].freeCategory==FreePointCategory::Daily,"old free points retain daily numbered behavior");
    Check(AutoRoute::RoutePlanStore::Parse(AutoRoute::RoutePlanStore::Document(legacy),"local","icons",none).skipHistory==legacy.skipHistory,"migration is stable when saved and read again");
    draft.Cancel();draft.Start(Scene1,StateIdFor(Scene1),FreePointCategory::Collectible,"collect");
    draft.Add(Blank(10,20,StateIdFor(Scene1)));draft.Undo();
    Rejects([&]{draft.Start(Scene1,StateIdFor(Scene1),FreePointCategory::Daily,"other");},"undoing every point does not unlock the chosen route type");
    Check(draft.Finish()&&draft.Pending(),"an emptied but locked draft can be resumed or discarded");
    draft.Start(Scene1,StateIdFor(Scene1),FreePointCategory::Collectible,"ignored");
    Rejects([&]{draft.SelectIcon(FreePointIcon::Monster);},"collectible routes reject non-number icons");
}
void FreeCompletionTests() {
    const auto root=std::filesystem::temp_directory_path()/std::to_string(GetTickCount64());
    std::int64_t epoch=100;
    ItemDatas daily=Blank(1,2,StateIdFor(Scene1));daily.itemId="free:1";
    daily.layer.stopKind=StopKind::Free;daily.freeRouteId="route-a";
    auto permanent=daily;permanent.itemId="free:2";permanent.freeCategory=FreePointCategory::Collectible;
    FreePointCompletionStore store(root,[&]{return epoch;});
    store.Set("local",daily,true);store.Set("local",permanent,true);
    Check(store.Completed("local",daily)&&store.Completed("local",permanent),"both free-point classes can be marked complete");
    auto other=daily;other.freeRouteId="route-b";
    Check(!store.Completed("local",other)&&!store.Completed("other",daily),"free completion is isolated by route and profile");
    ++epoch;
    Check(!store.Completed("local",daily)&&store.Completed("local",permanent),"daily expiry never erases collectible free-point completion");
    FreePointCompletionStore restarted(root,[&]{return epoch;});
    Check(restarted.Completed("local",permanent),"collectible free-point completion survives process restart");
    restarted.Set("local",daily,true);++epoch;
    FreePointCompletionStore afterClosedDay(root,[&]{return epoch;});
    Check(!afterClosedDay.Completed("local",daily)&&afterClosedDay.Completed("local",permanent),"daily completion resets even when the process was closed across the boundary");
    const auto heldFile=root/"profiles"/"local.free.json";
    const auto held=CreateFileW(heldFile.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    Rejects([&]{afterClosedDay.Set("local",daily,true);},"a failed atomic write refuses the completion");
    Check(!afterClosedDay.Completed("local",daily),"a failed write never advances in-memory completion");
    if(held!=INVALID_HANDLE_VALUE)CloseHandle(held);
    restarted.Set("local",permanent,false);
    Check(!restarted.Completed("local",permanent),"free-point completion can be undone");
    afterClosedDay.Set("local",daily,true);afterClosedDay.Set("local",permanent,true);
    afterClosedDay.Set("local",other,true);afterClosedDay.Set("other",daily,true);
    const auto deletionLock=CreateFileW(heldFile.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    Rejects([&]{afterClosedDay.RemoveRoute("local","route-a");},"failed deletion cleanup rejects its atomic write");
    Check(afterClosedDay.Completed("local",daily)&&afterClosedDay.Completed("local",permanent),"failed cleanup preserves both completion tables");
    if(deletionLock!=INVALID_HANDLE_VALUE)CloseHandle(deletionLock);
    const auto pruneLock=CreateFileW(heldFile.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    Rejects([&]{afterClosedDay.PruneRoute("local","route-a",{AutoRoute::Key(permanent)});},"failed edit cleanup preserves ledger");
    Check(afterClosedDay.Completed("local",daily)&&afterClosedDay.Completed("local",permanent),"failed pruning retains both point classes");
    if(pruneLock!=INVALID_HANDLE_VALUE)CloseHandle(pruneLock);
    afterClosedDay.PruneRoute("local","route-a",{AutoRoute::Key(permanent)});
    FreePointCompletionStore afterPrune(root,[&]{return epoch;});
    Check(!afterPrune.Completed("local",daily)&&afterPrune.Completed("local",permanent)&&afterPrune.Completed("local",other),"editing deletes only removed free identities, durably");
    afterClosedDay.RemoveRoute("local","route-a");
    FreePointCompletionStore afterDelete(root,[&]{return epoch;});
    Check(!afterDelete.Completed("local",daily)&&!afterDelete.Completed("local",permanent),"route deletion durably clears daily and permanent records");
    Check(afterDelete.Completed("local",other)&&afterDelete.Completed("other",daily),"route deletion preserves other routes and profiles");
    const auto file=root/"profiles"/"local.free.json";
    std::ifstream input(file);std::string bytes((std::istreambuf_iterator<char>(input)),{});
    Check(bytes.find("outbox")==std::string::npos&&bytes.find("pending")==std::string::npos,"local free progress has no synchronization records");
    // The test owns only this unique temp directory.
    input.close();
    std::error_code error;std::filesystem::remove(file,error);
    std::filesystem::remove(root/"profiles"/"other.free.json",error);
    std::filesystem::remove(root/"profiles",error);std::filesystem::remove(root,error);
}
void BadgeRenderingTests() {
    ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;
    io.DisplaySize={200,200};io.DeltaTime=1.0f/60;
    unsigned char* pixels=nullptr;int width=0,height=0;io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    ImGui::NewFrame();
    for(const auto* label:{"1","123","1C","3C"}) {
        ImDrawList draw(ImGui::GetDrawListSharedData());draw._ResetForNewFrame();
        draw.PushTextureID(io.Fonts->TexID);draw.PushClipRectFullScreen();
        DrawFreePointBadge(&draw,{100,100},label,false,true,false,12);
        Check(!draw.VtxBuffer.empty(),"completed free badges keep rendered geometry and their text");
        for(const auto& vertex:draw.VtxBuffer) {
            Check(std::hypot(vertex.pos.x-100,vertex.pos.y-100)<=13.05,
                "number and text icons fit the standard map marker and controller footprint");
            Check((vertex.col>>24)<=115,"completed circle and glyph vertices share the standard dimmed alpha");
        }
    }
    ImGui::EndFrame();ImGui::DestroyContext();
}

void FreeMarkerTests() {
    const std::array<AutoRoute::CircleClipVertex,3> edge={AutoRoute::CircleClipVertex{{90,-12},{0,0},{255,255,255,255}},
        AutoRoute::CircleClipVertex{{110,0},{1,0},{255,255,255,255}},AutoRoute::CircleClipVertex{{90,12},{0,1},{255,255,255,255}}};
    const auto clipped=AutoRoute::ClipTriangleCircle(edge,{0,0},100);
    Check(clipped.size()>=3,"edge badge triangles retain their visible portion");
    for(const auto& v:clipped)Check(std::hypot(v.position.x,v.position.y)<=100.00001&&v.uv.x>=0&&v.uv.x<=1&&v.uv.y>=0&&v.uv.y<=1,
        "circle clipping contains glyph vertices and interpolates texture coordinates");

    AutoRoute::HandDrawnDraft draft;draft.Start(Scene1,StateIdFor(Scene1));
    draft.Add(Blank(1,2,StateIdFor(Scene1)));draft.Add(Blank(3,4,StateIdFor(Scene1)));
    ConnectAll(draft);
    const auto plan=draft.Commit("marker-route","x","local");
    const auto remaining=AutoRoute::FreeMarkers(plan,{AutoRoute::Key(plan.stops[0])},1,7,true);
    Check(remaining.size()==1,"the last free target has its own marker snapshot independent of segments");
    if(!remaining.empty())Check(remaining[0].order==2&&remaining[0].current&&remaining[0].orderRevision==7,
        "a marker retains original order, target emphasis and revision");
    auto skipped=plan;skipped.skipped.insert(AutoRoute::Key(skipped.stops[1]));
    Check(AutoRoute::FreeMarkers(skipped,{AutoRoute::Key(plan.stops[0])},-1,8,true).empty(),
        "completed and skipped free stops produce no minimap markers");
    const auto mapMarkers=AutoRoute::FreeMarkers(skipped,{AutoRoute::Key(plan.stops[0])},-1,8);
    Check(mapMarkers.size()==2,"big-map free markers retain completed and skipped points for standard point operations");
    if(mapMarkers.size()==2)Check(mapMarkers[0].point.isSaved&&!mapMarkers[1].point.isSaved,
        "retained map markers carry completion separately from skipping");
}
void FarmWriteIdentityTests(){
    const auto root=std::filesystem::temp_directory_path()/("imao-farm-profile-"+std::to_string(GetCurrentProcessId()));
    FarmCompletionStore store(root,[]{return 100;});store.SelectProfile("other");
    Rejects([&]{store.SetForProfile("local",1,"target",true);},"stale farm profile write rejected under ledger lock");
    Check(!store.Completed(1,"target"),"stale write never marks the new profile");
    store.SelectProfile("local");store.SetForProfile("local",1,"target",true);
    Check(store.Completed(1,"target"),"matched farm profile persists completion");
    const auto held=CreateFileW(store.Path("local").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    Rejects([&]{store.SetForProfile("local",1,"failed",true);},"farm write failure is reported");
    Check(!store.Completed(1,"failed"),"farm failed write cannot advance memory");
    if(held!=INVALID_HANDLE_VALUE)CloseHandle(held);std::filesystem::remove_all(root);
}
void CollectionOrderTests(){
    const auto root=std::filesystem::temp_directory_path()/("imao-hand-order-"+std::to_string(GetCurrentProcessId()));
    AutoRoute::RouteCollections store(root);AutoRoute::RouteCollections::Index index;
    index.orders["default"]={"b","a"};index.autoRotate["default"]=true;index.orderRevision=7;
    store.Save("local",index);const auto saved=store.Load("local");
    Check(saved.orders.at("default")==std::vector<std::string>{"b","a"}&&saved.orderRevision==7&&saved.autoRotate.at("default"),"default collection order and rotation survive restart");
    Check(store.Load("other").orders.empty(),"collection order is profile isolated");
    WriteTextAtomically(store.Path("local"),"{ broken index");const auto damaged=store.Load("local");
    Check(!damaged.writable,"unreadable collection index is read-only");
    Rejects([&]{store.Save("local",damaged);},"automatic reconciliation cannot overwrite damaged collection bytes");
    std::ifstream bytes(store.Path("local"));std::string retained((std::istreambuf_iterator<char>(bytes)),{});bytes.close();
    Check(retained=="{ broken index","damaged index remains recoverable");
    std::filesystem::remove_all(root);
}
} // namespace

int main() {
    try {
        Check(AutoRoute::HandDrawingInputAllowed(true,1,1,true,true),"a fresh focused original map owns hand drawing input");
        Check(!AutoRoute::HandDrawingInputAllowed(true,1,1,false,true),"a suspended drawing cannot swallow another application's Escape or undo");
        Check(!AutoRoute::HandDrawingInputAllowed(true,1,1,true,false),"an expired map cannot claim hand drawing input");
        Check(!AutoRoute::HandDrawingInputAllowed(true,1,2,true,true),"a different map cannot claim the retained drawing's input");
        Check(!AutoRoute::HandDrawingInputAllowed(false,1,1,true,true),"an inactive drawing does not own the keyboard");
        Check(!AutoRoute::HandDrawingInputAllowed(true,0,0,true,true),"an unknown map cannot claim the keyboard");
        FarmWriteIdentityTests(); CollectionOrderTests(); EditorInputTests(); EditorTests(); DraftTests(); ImportTests(); FreeIdentityTests(); FreeIconAndMigrationTests(); FreeCompletionTests(); FreeMarkerTests(); BadgeRenderingTests();
    }
    catch (const std::exception& error) { ++failures; std::cerr << "UNEXPECTED: " << error.what() << '\n'; }
    if (failures) { std::cerr << failures << " hand-drawn route test(s) failed\n"; return 1; }
    std::cout << "Hand-drawn route tests passed\n";
    return 0;
}
