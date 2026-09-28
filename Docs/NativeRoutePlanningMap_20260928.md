# Native (C++) Route-Planning Core — Evidence Map

Scope: `C:\Dcode\WWMAP-TOOLS` (read-only investigation; no file was modified, nothing was built).
All paths are relative to the repo root. Line numbers are from the current working tree at the time of writing.

Legend for "could not determine" items: **UNKNOWN**.

---

## 0. Executive summary / architecture in one screen

Two *completely separate* route concepts live side by side:

| | Automatic route (自动路线 / 路线规划) | Hand-drawn route (手绘路线) |
|---|---|---|
| Owner | `RoutePlanningService` (`Runtime/RoutePlanningService.cpp`) | `LoadEditRouteData` (`ImguiDraw/Routes/LoadEditRouteData.cpp`) |
| Data unit | ordered list of **POI identities** (`AutoRoute::Plan::stops`, each an `ItemDatas`) | list of **raw line segments** between two mouse-picked points (`RouteDatas`) |
| Solver | nearest-neighbour + 2-opt, `AutoRoute::Solve` (`Runtime/RoutePlanningModel.h:59`) | none |
| Storage | `%LOCALAPPDATA%\IMao-WinUI\SavedRoutes\Auto\<profileId>\<routeId>.json` + `active.json` | `%LOCALAPPDATA%\IMao-WinUI\SavedRoutes\<name>.json` (flat) |
| Persistence code | `Runtime/RoutePlanStore.h` | `LoadEditRouteData.cpp:195` (`WriteRoutesDatas`) |
| UI command surface | JSON `{"type":"routePlanning","action":..., ...}` over the CoreHost pipe | WinUI-only: `setRouteName` / `loadRoutes` / `loadRoute` pipe types (one-way) |
| Drawing | `App.cpp:2681-2722` builds `RouteDatas` segments → `DrawRouteOnMap::DrawRoute` / `DrawRouteOnMinMap::DrawRoute` | same two draw functions, `automatic == false` branch |
| Shares storage? | **No.** No code path reads or writes both trees. Confirmed by grep: `RoutePlanStore` only ever points at `.../SavedRoutes/Auto` (`RoutePlanningService.cpp:382`); `LoadEditRouteData::RouteFolderPath()` only ever points at `.../SavedRoutes` (`LoadEditRouteData.cpp:24-36`). |

The single write of a "hand-drawn" style segment from the *automatic* feature is the preview/previous-target dashed polyline, which is still `automatic = true` (see §5).

---

## 1. Command / action protocol between UI and the route-planning service

### 1.1 Transport

* C++ entry point: `CoreHostMain.cpp:504-509`
  ```cpp
  if (type == "routePlanning") {
      const auto result = RoutePlanningService::Command(command);
      SendAck(events, command, result.value("accepted", false), result.value("message", ""),
              result.value("data", json::object()));
  ```
  So the *whole* JSON command object is handed to `Command`; `type` is `"routePlanning"`.
* C# producer: `IMao-WinUI/Services/CoreHostService.cs:410-439` — `ExecuteRoutePlanningAsync(action, arguments)`:
  ```csharp
  command["type"] = "routePlanning";   // :421
  command["action"] = action;          // :422
  command["version"] = ProtocolVersion;// :423
  command["requestId"] = id;           // :424
  ```
  15-second timeout (`:428`), reply is the ack `data` → `RoutePlanningState.FromJson` (`:434`, model in `IMao-WinUI/Models/RoutePlanningState.cs:39`).
* Push channel (unsolicited): `RoutePlanningService.cpp:173-179` `Emit()` publishes `{"type":"routePlanningChanged","data":SnapshotLocked()}`; consumed at `CoreHostService.cs:573-576`.

### 1.2 Parse / dispatch site

`RoutePlanningService::Command` — **`IMao-Core/src/Runtime/RoutePlanningService.cpp:583-713`**.
Action is read at **line 591**: `const auto action = command.value("action","state");`
The `if/else` chain runs **598 → 709**; the `else` at **709** rejects anything unknown with `"未知的自动路线操作"`.

### 1.3 Common (pre-action) gate fields — lines 586-597

| field | line | semantics |
|---|---|---|
| `profileId` | 587 | must equal the live marker profile, else `"档案已变化，请刷新路线"` |
| `expectedRevision` | 588 | must equal `r.revision` |
| `expectedSceneId` | 589 | must equal `r.scene` |
| `expectedGeneration` | 590 | must equal `r.epoch` |
| `routeId` | 592-593 | required to match the active route for *every* action except `load` and `delete` |
| `key` | 594-597 | for `complete`/`skip`/`guide`: must equal `AutoRoute::Key(active->stops[TargetIndexLocked()])` |

On success: `{"accepted":true,"message":...,"data":SnapshotLocked()}` (`:710`).
On exception: `{"accepted":false,"message":<e.what()>,"data":SnapshotLocked()}` (`:711`), and `r.message` is set too.

### 1.4 Every action, its payload and its full effect

| action | line | payload beyond the common gate | effect |
|---|---|---|---|
| `state` | falls through to 709 (`action!="state"` guard) | – | no-op refresh; still bumps `++r.revision` at 710 and emits |
| `new` | **598-601** | `sceneId` (optional, defaults to `r.observedScene`) | `InvalidateLocked()`, `enabled=true`, `tool="pan"`, resets/creates the draft for that scene |
| `end` | **602** | – | `enabled=false`, `tool="pan"`, message `"已退出选点，草稿保留"` |
| `tool` | **603-605** | `tool` ∈ {`pan`,`box`,`rectangle`,`lasso`,`point`,`start`} | `rectangle` is normalized to `box` (`:605`); invalid → `"选点工具无效"` |
| `setStart` | **606-609** | `sceneId`, `x`, `y` (ROC/identify coords) | writes `draft.start = {r.scene, roc, "manual", 0, r.epoch, true}`; `tool="pan"`, preview dropped |
| `add` | **610-614** | `keys` : array of `"<stateId>:<pointId>"` | `AddLocked(keys)` |
| `addVisible` | **610-614** | – | uses `r.visible`, filtered by `AutoRoute::IsSurfaceTarget` (i.e. skips layered points) |
| `toggle` | **615-619** | `key` | remove if present, else `AddLocked({key})` |
| `remove` | **615-619** | `key` | remove only (no add fallback) |
| `undo` | **620-621** | – | pops `draft.history` (max 100 entries, see `RememberSelectionLocked` `:195-198`) |
| `clear` | **622-623** | – | clears `draft.selected` with one undo step |
| `generate` | **624-625** | – | `QueueSolveLocked(false)` |
| `replan` | **624-625** | – | `QueueSolveLocked(true)`; if no map observed, `r.scene = r.active->sceneId` first |
| `activate` | **626-635** | – | requires `draft.preview`; `store->Save(*preview, true)`; becomes `r.active`, `runRequested=true`, `enabled=false`; farm mode adopted from the plan (`:632`) |
| `stop` | **636-640** | – | `store->ClearActive(profile)` **then** `StopNavigationLocked()` |
| `delete` | **641-652** | `routeId` | `store->Delete`; `DeleteRollbackFailure` → also stops navigation then rethrows; clears any draft preview with that id; rescans list |
| `pause` | **653** | – | `runRequested=false`, `InvalidateAutoLocked()`, route stays visible/loaded |
| `resume` | **654-655** | – | requires `r.active`; `runRequested=true` |
| `skip` | **656-664** | `key` (must match current target, gated at 594) | inserts key into `plan.skipped`, pushes to `skipHistory`, `store->Save(next,false)`, becomes active, bumps `orderRevision` |
| `undoSkip` | **656-664** | – | pops `skipHistory`, erases from `skipped`, saves |
| `farm` | **665-678** | `enabled` (optional, defaults to `!r.farmMode`) | writes `plan.farmMode` to disk first, then flips `r.farmMode` |
| `guide` | **679-686** | `key` (gated) | unlocks the mutex, calls `DrawItemBase::SelectMarker(scene,item,cursor,profile)` → emits `markerSelected` |
| `complete` | **687-693** | `key` (gated) | unlocks, calls `DrawItemBase::HandleMarkerCommand({type:"markerSetCompletion", ..., "completed":true})` |
| `save` | **694-700** | `target` (`"preview"` or active), `name` | `plan.name = command.value("name", plan.name)`; `store->Save(plan, !preview)`. **This is the rename path for automatic routes** — note it rewrites in place: the file name (`<id>.json`) is unchanged (see `RoutePlanStore::Path` `:133-138`), only the display name changes. |
| `load` | **701-707** | `routeId` | `store->Load(...)` then `store->Save(next,true)` (re-points `active.json`); `runRequested=false`, so the player must then send `resume` |
| `list` | **708** | – | refresh `r.saved` from `store->List(profile)` |

Internal helpers that construct these same commands (no separate protocol):
* `AddPoints(points, context)` — `:576-579` → `{"action":"add","keys":[...]}`
* `TogglePoint(point, context)` — `:580` → `{"action":"toggle","key":Key}`
* `SetManualStart(scene, roc, context)` — `:581-582` → `{"action":"setStart",...}`
* `GuideTarget(command)` — `:395-419`, a *separate* response path used by the F8 guide hotkey (`type == "markerGetRouteGuide"`, `CoreHostMain.cpp:657`).

### 1.5 Where the UI turns button ids into actions

* ImGui route toolbar (mostly dead, see §1.6): `DrawMarkerInteraction.cpp:1602-1640`. Action string is derived mechanically: `command["action"] = click.target.substr(6);` (`:1634`) — i.e. `"route:skip"` → `"skip"`. Tool ids use `substr(11)` (`:1629`) — `"route:tool:box"` → `"box"`.
* Farming has its own branch before that: `:1605-1611` (`route:farm:on|off` → `{"action":"farm","enabled":…}`).
* `route:addGroup` (`:1619-1625`) → `AddPoints(...)`.
* WinUI gamepad tools window: `IMao-WinUI/Services/MapToolsController.cs:319-403`. Notable translations:
  * `tool:edit` → `{"action":"tool","tool":"pan"}` (`:364`)
  * `tool:pan` closes the window instead of sending a command (`:385`)
  * payload receives `expectedSceneId`/`expectedGeneration` for everything except `new`,`resume`,`pause`,`stop`,`undoSkip`,`farm` (`:373-374`)
* WinUI desktop route page buttons: `IMao-WinUI/Views/FunctionPage.xaml.cs:221-259` (also `complete`/`skip`/`guide` send `key`+`routeId`, `:224-231`).

### 1.6 `BuildPlanningToolbar` is DEAD code — confirmed

* Definition: `DrawMarkerInteraction.cpp:607` (`ToolbarUi BuildPlanningToolbar(const RoutePlanningView&, const RECT&)`).
* The only other two mentions are its own comment block (`:601-606`) and `PlanningPanel` (`:697-699`), which is itself uncalled.
* Whole-repo grep for `BuildPlanningToolbar|DrawPlanningToolbar|PlanningPanel` returns **only** those 5 hits inside `DrawMarkerInteraction.cpp` (lines 601, 607, 697, 698, 711). No call sites anywhere, including tests.
* The comment at `:601-606` states the reason explicitly: the toolbar moved to the WinUI tools window (`IMao-WinUI/Views/MapToolsWindow.cs:173 RenderRoute`), and editing only the C++ copy silently does nothing on real hardware.
* Live toolbar button list therefore is `MapToolsWindow.RenderRoute` `:184-211`; the leftover C++ list is `:619-683`.

**UNKNOWN:** whether `DrawGesturePreview`'s green selection visuals (`:748-795`) are the only remaining per-frame route UI in C++; they are reachable (called at `:1420`).

---

## 2. Exact data model of a route plan

### 2.1 In memory

`IMao-Core/src/Runtime/RoutePlanningModel.h:16-36`:

```cpp
struct Start {                       // :16
    int sceneId = 0;
    Coordinate roc;                  // ROC = identify/map coordinate space
    std::string source = "playerSnapshot";   // or "manual", "autoPlayerFix"
    std::int64_t confirmedUnixMs = 0;
    std::uint64_t generation = 0;
    bool valid = false;
};
struct Plan {                        // :24
    std::string id, name, profileId;
    int sceneId = 0;
    Start start;
    std::vector<ItemDatas> stops;
    std::unordered_set<std::string> skipped;
    std::vector<std::string> skipHistory;
    bool farmMode = false;           // :35
};
```

`SolveResult` — `:37-41` (`stops`, `initialLength`, `planarLength`, `cancelled`).
`DrawVisibility` — `:42-54` (profileId/activeId/previewId/navigating/orderRevision/comparisonVisible + `Allows()`).

A "stop"/target is an `ItemDatas` — **`IMao-Core/src/Domain/MapData.h:16-23`**:

```cpp
struct ItemDatas {
    std::string itemId;          // stable point id ("pointId" in JSON)
    std::string nameId;          // category id (icon + display name source)
    Coordinate screenCoordiante; // only filled for rendering frames
    Coordinate itemMapROC;       // the route-relevant coordinate
    bool isSaved = false;
    MapLayerIdentity layer;      // :8-14 {stateId, countryId, floorId, level}
};
```

* Identity key: `AutoRoute::Key(item) = std::to_string(item.layer.stateId) + ":" + item.itemId` (`RoutePlanningModel.h:13-15`).
* Coordinate space: `itemMapROC` is the **ROC** ("identify") coordinate produced by `RelativeCoordinates::IdentifyCoordToROC` (`RoutePlanningService.cpp:369` for the catalog) or by `RelativeCoordinates::ImgMapCoordToROC` (for manual start, `DrawMarkerInteraction.cpp:820`). Solver distances are plain Euclidean in that space (`RoutePlanningModel.h:83` `std::hypot`).
* There is **no icon field** on a stop; the icon comes from `nameId` via `DrawItemBase::itemTextureIndex` / `GetExternalIconPath` (`DrawItemBase.h:39`, used at `DrawMarkerInteraction.cpp:1241-1256`). The Chinese category label comes from `R().names[nameId]`, built in `BuildCatalogLocked` (`RoutePlanningService.cpp:359-377`, used in `StopJsonLocked` `:136-138`).

### 2.2 Snapshot / wire JSON (what the UI sees)

`SnapshotLocked()` — `RoutePlanningService.cpp:156-172`:

```
profileId, sceneId, sceneName, enabled, tool, computing, revision, generation,
message, selectedCount, hiddenCount,
start{...}, selected:[stop...], preview: plan|null, active: plan|null,
navigationStatus, currentTarget: stop|null,
autoReplanEnabled, autoReplanComputing, autoReplanStatus,
farmMode, farmNotice, farmNoticeSerial,
orderRevision, previousTarget: stop|null, savedRoutes:[...]
```

`StopJsonLocked` — `:135-141`:
```cpp
return {{"key",Key(item)},{"stateId",item.layer.stateId},{"pointId",item.itemId},{"nameId",item.nameId},
    {"name",<category name or nameId>},{"x",item.itemMapROC.x},{"y",item.itemMapROC.y},
    {"countryId",item.layer.countryId},{"floorId",item.layer.floorId},{"level",item.layer.level},
    {"completed",...},{"skipped",skipped},{"order",order}};
```
`PlanJsonLocked` — `:142-151`: adds `id, name, profileId, sceneId, sceneName, start, stops, planarLength`. Note `planarLength` here is recomputed over *remaining* stops only (`:146-147`).

C# mirror: `IMao-WinUI/Models/RoutePlanningState.cs:6-126` (`RoutePlanningState`, `RouteStart`, `RouteStop`, `AutomaticRoute`, `SavedAutomaticRoute`).

### 2.3 On-disk JSON

`RoutePlanStore::Save` — `IMao-Core/src/Runtime/RoutePlanStore.h:32-47`:

```cpp
Json stops=Json::array();
for(const auto& p:plan.stops) stops.push_back({{"stateId",p.layer.stateId},{"pointId",p.itemId},
    {"nameId",p.nameId},{"x",p.itemMapROC.x},{"y",p.itemMapROC.y},{"countryId",p.layer.countryId},
    {"floorId",p.layer.floorId},{"level",p.layer.level},{"skipped",plan.skipped.contains(Key(p))}});
const Json doc={{"formatVersion",1},{"id",plan.id},{"name",plan.name},{"profileId",plan.profileId},
    {"sceneId",plan.sceneId},{"start",StartJson(plan.start)},{"stops",std::move(stops)},{"skipHistory",plan.skipHistory},
    {"farmMode",plan.farmMode}};
```
`StartJson` — `:10-13`: `{valid, sceneId, x, y, source, confirmedUnixMs, generation}`.

Deliberately **not** persisted: any `completed`/`isSaved` flag (test asserts it, `tests/RoutePlanningTests.cpp:195-196`); completion is authoritative from the marker store.

---

## 3. Persistence / load (RoutePlanStore)

All in `IMao-Core/src/Runtime/RoutePlanStore.h` (header-only class) except the directory choice.

### 3.1 Root path

* Constructed once at init: `RoutePlanningService.cpp:382`
  ```cpp
  r.store=std::make_unique<AutoRoute::RoutePlanStore>(StructuredLogger::ApplicationDataDirectory()/"SavedRoutes"/"Auto");
  ```
* `StructuredLogger::ApplicationDataDirectory()` = `%LOCALAPPDATA%\IMao-WinUI` (`StructuredLogger.cpp:44-51`).
* Final pattern: **`%LOCALAPPDATA%\IMao-WinUI\SavedRoutes\Auto\<profileId>\<routeId>.json`** plus **`...\<profileId>\active.json`** (`Folder()` `:132`, `Path()` `:133-138`).
* C# mirrored path list (kept in sync for the file-sandbox): `IMao-WinUI/Services/CoreHostService.cs:45` uses `Path.Combine(UserDataPaths.Root, "SavedRoutes", "Auto")`; `IMao-WinUI/Helpers/UserDataPaths.cs:5-6`.

### 3.2 What identifies a plan

* **`id` is the identity** (a lowercase UUIDv4 produced by `NewId()` — `RoutePlanningService.cpp:87-95`, `BCryptGenRandom`). It is the on-disk file name.
* `name` is display-only and editable via `{"action":"save","name":...}` (§1.4).
* `profileId` is the parent directory; validated to `[A-Za-z0-9_-]{1,96}` (`ValidateRouteComponent` `:14-17`). `Path()` refuses the reserved id `active` case-insensitively (`:135-136`) and lowercases the file name (`:135`), while comparisons use `SameRouteId` (case-insensitive, `:18-21`).

### 3.3 Capabilities already present

| capability | code | notes |
|---|---|---|
| Save (create/overwrite) | `Save` `:32-47` | atomic write via `WriteTextAtomically` (`Runtime/AtomicFile.h`) |
| Save + make active | `Save(plan, true)` `:45-46` | writes `active.json` = `{"formatVersion":1,"routeId":id}` |
| Load one | `Load(profile,id,resolver)` `:48-74` | re-resolves every stop against the live catalog and refuses to load if `nameId` or coordinates changed (`:64-66` → `"点位资源已变化，请重新规划：<key>"`) |
| Load active | `LoadActive` `:75-87` | honours the deletion tombstone: `if(!exists(routePath)&&exists(DeletingPath(routePath)))return {};` (`:85`) |
| Clear active | `ClearActive` `:88-90` | writes `routeId: null` |
| **Delete** | `Delete` `:91-115` | rename-to-`.deleting` **is** the commit (`:103-104`); rollback on failure to clear the pointer (`:105-110`); tombstone then best-effort removed (`:113-114`) |
| **List** | `List` `:116-128` | skips `active.json`, tolerates corrupt files as `"<id>（文件损坏）"`, sorts by id, returns `{id,name,sceneId,sceneName}` |
| **Rename** | *no dedicated action* | achieved by `save` with `name` (§1.4); the id/file name never changes |
| File-size guard | `Read` `:139-143` | >2 MiB or missing → `"无法读取自动路线文件"` |
| Schema/name validation | `Validate` `:144-157` | name ≤256 chars, known scene, start valid & same scene, 1..500 stops, unique keys, skips ⊆ stops, history ⊆ skips |

Behaviour on activate/load (`action == "load"`, `RoutePlanningService.cpp:701-707`):
`Load` → `Save(next,true)` (re-points active) → `InvalidateLocked()` → `active=next` → **`runRequested=false`** (deliberately paused; UI message `"路线已加载，点击继续导航"`) → farm mode adopted from the file → drafts' matching preview dropped → `RefreshCompletedLocked()`.
On `activate` (`:626-635`) the same happens but `runRequested=true` and navigation starts immediately.
On process start, `SyncProfileLocked()` (`:180-194`) calls `List` + `LoadActive` and, if a route is restored, sets `message="已恢复自动路线，点击继续导航"` and adopts `farmMode` — again paused.

### 3.4 Related but separate: legacy/hand-drawn store

* Root: `StructuredLogger::ApplicationDataDirectory() / "SavedRoutes"` (`LoadEditRouteData.cpp:24-36`), with a one-time migration copy from the **program directory** `SavedRoutes\*.json` (`:28-32`).
* File name validated by `ValidateUserFileName` (`Runtime/UserFileName.h`, `RoutePath` `:38-41`).
* JSON shape is `{ "<SceneName>": [ [[x,y],[x,y]], ... ] }` (`WriteRoutesDatas` `:195-213`, `LoadRoutesDatasFromLocal` parsing `:163-193`).
* C#-side pipe commands: `setRouteName`, `loadRoutes`, `loadRoute` (`CoreHostMain.cpp:737-751`), wrappers `CoreHostService.cs:406-408`, UI `FunctionPage.xaml.cs:105-125`, `FunctionPage.xaml:64-72`.

---

## 4. Hand-drawn route feature, end-to-end

### 4.1 Input path

* Key: `RuntimeHotkeyBindings::manualRouteKey = 81` (VK_Q) — default in `RuntimeHotkeys.h:11`, persisted/validated at `:73-80`, labelled 手绘端点 in `SettingsPage.xaml.cs:686` and described in `FunctionPage.xaml.cs:63-64`.
* Polling loop: `LoadEditRouteData::Thread_KeyMonitoring_AddRouteDatas_ByMousePos()` — **`LoadEditRouteData.cpp:86-141`**. Started by `Initi` → `StartThread()` (`:52-69`, `LoadEditRouteData.h:41-44`).
* Behaviour: first fresh press stores point A (`:123-129`), second fresh press stores B and commits (`:130-137`). Modifier-held presses are ignored (`:121-123`). Any scene change resets state to "press A again" (`:125-128`).
* **Mutual exclusion with the automatic planner** — `:105-111`:
  ```cpp
  // Route planning owns its own draft and undo history. Legacy Q must
  // not append a separate hand-drawn segment during the same session.
  if (monitoredKey == 0 || RoutePlanningService::PlanningMode()) { state = 0; Sleep(60); continue; }
  ```
  `PlanningMode()` is `RoutePlanningService.cpp:442` (returns `R().enabled`).
* Mouse position → map coordinate: `App::TryGetRoutePoint(Coordinate&, int& sceneId)` — **`App.cpp:2504-2521`**. It reads the *presented* overlay frame, requires `Fresh() && mapVisible && AllowsMap(...) && IsWindowFocused(hwnd) && pixelsPerUnit>0`, inverts the overlay motion, divides by `pixelsPerUnit`, then `RelativeCoordinates::ImgMapCoordToROC(mapPoint, sceneId)` (`:2519`). `App::GetMapCoordinatesOfMousePos()` (`:2496-2502`) adds the scene origin back.

### 4.2 Data created

`AddRouteDatas(name, senceId, ROC_a, ROC_b)` — `LoadEditRouteData.cpp:71-78`:
```cpp
vector<Coordinate> routePointsROC = GenerateEquidistantPoints(ROC_a, ROC_b, 5);
{ std::scoped_lock lock(dataMutex);
  LoadEditRouteData::routesDatas.push_back(RouteDatas(name, senceId, std::move(routePointsROC))); }
DrawRouteOnMap::ClearRountsData();
```
So one hand-drawn "route" is a straight segment stored as **5 equidistant interpolated points** (from `util.h`), pushed into a global `std::vector<RouteDatas>` (`:17`). Each Q-pair appends a new segment; there is no grouping into named multi-segment objects in memory — the on-disk file groups them only by scene name.

### 4.3 Rendering & style

* Screen projection: `DrawRouteOnMap::GetRoutePointsScreen` (`DrawRouteOnMap.cpp:12-30`) and `DrawRouteOnMinMap::GetRoutePointsScreen` (`DrawRouteOnMinMap.cpp:13-30`), called from `App.cpp:369` and `App.cpp:527`.
* Style constants (both draw functions, identical values): `DrawRouteOnMap.cpp:58-60`, `DrawRouteOnMinMap.cpp:58-60`:
  ```cpp
  const ImU32 color = !routeDatas.automatic ? IM_COL32(255, 0, 0, 255) : ...
  const float thickness = routeDatas.emphasized ? 3.5f : routeDatas.automatic ? 2.0f : 1.5f;
  ```
  → hand-drawn = **opaque pure red, 1.5 px, solid (no dash)**. No arrowheads, no stop markers, no start/end glyph for hand-drawn routes. The same red is used identically on the minimap (`DrawRouteOnMinMap.cpp:58-60`).
* Visibility gate: `RoutePlanningService::DrawingVisibility()` → `AutoRoute::DrawVisibility::Allows` — `RoutePlanningModel.h:47-53`:
  ```cpp
  bool Allows(const RouteDatas& route, bool minimap = false) const {
      if (!route.automatic) return true;      // ← hand-drawn routes are always drawn
      ...
  }
  ```
* Hand-drawn routes are **not** gated by `enabled`/`active`/profile; only the `senceId` match in the draw loop (`DrawRouteOnMap.cpp:46-48`, `DrawRouteOnMinMap.cpp:44-45`) applies.

### 4.4 Sharing storage with auto-planned routes?

**No.** Hand-drawn segments never enter `AutoRoute::Plan`, never go through `RoutePlanStore`, and never appear in `savedRoutes` of the snapshot. Conversely, `RouteDatas` produced by `App.cpp:2681-2722` always set `automatic = true`, so they can never be mistaken for hand-drawn ones. The only coupling is the runtime guard at `LoadEditRouteData.cpp:107`.

---

## 5. Automatic (auto-planned) route rendering

### 5.1 Pipeline

1. **Plan → segment list** (per frame): `App.cpp:2681-2705` `appendRoute(plan, preview)`.
   ```cpp
   RouteDatas segment(plan.name, plan.sceneId, {previous, stop.itemMapROC}, {project(previous), project(stop.itemMapROC)});
   segment.automatic = true; segment.preview = preview; segment.emphasized = !preview && first;   // :2698
   segment.profileId = plan.profileId; segment.routePlanId = plan.id;                              // :2699-2700
   segment.orderRevision = routeView.orderRevision;                                               // :2701
   ```
   Starts from `routeView.mapStart.roc` (or the plan start), skips completed/skipped stops (`:2692`), published into `frame.mapRoutes` / `frame.minimapRoutes` (`:2702`, `FrameState.h:32`).
   The "previous target" hint is a separate pseudo-segment: `App.cpp:2707-2721`, `hint.previousTarget = true`.
   Preview is only appended when `routeView.enabled && routeView.preview` (`:2722`).
2. **Snapshot into the drawer**: `App.cpp:2678` `frame.mapRoutes = DrawRouteOnMap::Snapshot();` (mutex-protected copy, `DrawRouteOnMap.cpp:73`).
3. **Draw**: `ImGuiOverWindows.cpp:824` (map) and `:845` (minimap) → `DrawRouteOnMap::DrawRoute` (`DrawRouteOnMap.cpp:32-71`) / `DrawRouteOnMinMap::DrawRoute` (`DrawRouteOnMinMap.cpp:34-71`).
4. **Visibility filter**: `AutoRoute::DrawVisibility::Allows` (`RoutePlanningModel.h:47-53`): requires matching `profileId` + non-empty `routePlanId`; previews only on the big map (`!minimap`) and only when `routePlanId == previewId`; active only when `routePlanId == activeId && orderRevision matches && (!previousTarget || comparisonVisible) && (!minimap || navigating)`.

### 5.2 Exact style constants

`DrawRouteOnMap.cpp:51-69` (big map):

| case | colour | width | pattern |
|---|---|---|---|
| hand-drawn (`!automatic`) | `IM_COL32(255,0,0,255)` | 1.5 | solid |
| `previousTarget` | `IM_COL32(172,180,190,195)` (grey, translucent) | 2.0 | **dashed, 14 px period, 8 px dash** (`:63-64`) |
| `preview` | `IM_COL32(102,201,222,190)` | 2.0 | **dashed, 14 / 8** (`:63-64`) |
| `emphasized` (first leg of the active route) | `IM_COL32(255,193,73,255)` (amber) | **3.5** | solid |
| active, other legs | `IM_COL32(81,168,209,210)` (blue) | 2.0 | solid |

Clipping: `AutoRoute::ClipRectangle` (`RouteGeometry.h:41-57`) per original segment (`DrawRouteOnMap.cpp:54`), applied after the frame motion transform `motion.Apply` (`:52-53`).

Minimap differences — `DrawRouteOnMinMap.cpp:51-68`:
* first leg for `emphasized`/`previousTarget` starts at `clipCenter` instead of the projected point (`:51-52`);
* clipping uses `AutoRoute::ClipCircle` (`:54`, impl `RouteGeometry.h:59-77`);
* **dash period is 12 / 7** (`:63-64`) and emphasized width is **3.0** instead of 3.5 (`:60`).

### 5.3 Stop markers, start marker, "current target" marker

All drawn in `DrawMarkerInteraction::DrawMap` under `if (planningBinding.valid)` — **`DrawMarkerInteraction.cpp:1548-1584`**:

* **Start marker** (selection mode only): a 12 px filled disc `IM_COL32(19,92,78,250)` + 14 px ring `IM_COL32(110,250,190,255)` + the glyph `"起"` — `:1552-1561`.
* **Numbered stop badges**: for preview or active plan, skipped/completed stops are skipped (`:1568`); only the first badge in each 22×22 px bucket is drawn (`:1571-1575`, guards overlapping labels); label = `index+1`; badge radius `max(10, size/2+4)`; colour = current target `IM_COL32(233,165,57,255)` (amber) else `IM_COL32(31,114,151,245)` (blue) — `:1576-1581`.
* The badge is drawn *beside* the POI icon (x offset `radius+12`, `:1578`); the POI icon itself is drawn by the normal marker layout (`DrawIcon`, `:1440-1441`), with the "selected" highlight colour `IM_COL32(67,226,138,255)` (`:1261`).
* **No arrowheads anywhere** in the route drawing code.
* Selection-mode grouping UI (the "N selected" pill): `:1449-1456`.

---

## 6. Screen position → map point, hover, and "is there a POI here?"

### 6.1 Resolution chain (screen → semantic point)

1. Hit regions are built each frame into `regions` (`std::vector<MarkerHitRegion>`) and looked up by desktop client coordinates: `Hit(double x, double y)` — `DrawMarkerInteraction.cpp:259-267` (reverse iteration = topmost first). Region keys:
   * `"p:<pointId>"` = a single marker (added at `:1457` for the group anchor, `:1518` / `:1520` for expanded members);
   * `"g:<pointId>"` = a group (multi-marker pile) anchor (`:1428`, `:1434`);
   * `"panel"`, `"page:next"`, `"page:previous"`, `"maptools:open"`, and `route:*` for UI chrome (`:721`, `:736-737`, `:1490`, `:1533-1534`, `:1543`, `:1221`).
   Region insertion helper `AddRegion(...)` — `:584-589`.
2. Region key → identity string: `PointKey(item) = std::to_string(item.layer.stateId) + ":" + item.itemId` — `:540` (duplicated as `AutoRoute::Key`).
3. Region key → route-selection target: `AutoRoute::RouteSnapSelectionTarget(hit)` — `Runtime/RoutePointSelectionInput.h:42-46` maps `"g:X"` → `"p:X"` and rejects non-point hits.
4. Screen → map-image: `DrawMarkerInteraction::ScreenToMapImage(binding, point)` — `:162-167` (inverse of `MapImageToScreen` `:156-161`; both use `binding.presented.motion` and `source.mapMotion.pixelsPerUnit`).
5. Map-image → ROC: `RelativeCoordinates::ImgMapCoordToROC(...)` — used for manual start at `:820`.
6. Standalone (non-hook) variant used by the hand-drawn feature: `App::TryGetRoutePoint` (`App.cpp:2504-2521`), described in §4.1.

### 6.2 Hover state

Hover is computed geometrically per frame, not via `regions`:
* `const bool hover = std::hypot(group.anchor.x - mouseX, group.anchor.y - mouseY) <= radius + 4;` — `:1435` (mouse position from `GetCursorPos` at `:1302-1303`).
* Hover is remembered as a group key and promoted to "expanded" after 350 ms: `:1461-1466`.
* Expanded member hover: `:1502-1503`.

### 6.3 "Is there a POI at this map position, and which one?"

There is **no map-space point query**. The available queries are:

| question | function |
|---|---|
| Which region is at this *desktop* pixel? | `Hit(x,y)` `:259-267` |
| What is the point at this screen pixel, for route selection? | `SnapRouteCursorToNearest` `:269-281` + `AutoRoute::NearestRouteSnapPoint` `RoutePointSelectionInput.h:59-67` |
| Which points are inside a box/lasso (screen space)? | `GestureMatches` `:219-229` + `AutoRoute::PointInPolygon` `RouteGeometry.h:25-38` |
| Which markers are in the current viewport / on the unobstructed canvas? | `AutoRoute::ViewportCandidates::Add` `RouteViewportCandidates.h:14-24`; filled at `DrawMarkerInteraction.cpp:1318-1323` |
| Resolve an id string → `ItemDatas` (route service side)? | `ResolveLocked(scene, key)` `RoutePlanningService.cpp:112-115` against the catalog built by `BuildCatalogLocked` `:359-377` |
| Resolve a *sceneName+pointId* from the UI at click time? | local `visible` map (`:1361`, `:1376`, lookup `:1648`) |
| Gamepad-cursor specific hit test / candidate resolution | `GamepadCursorTargets` (`Runtime/GamepadCursorTargets.h`), read via `CoreHostMain.cpp:329`, `ReadGamepadContext()`; `GamepadCursorGeometry` (`Runtime/GamepadCursorGeometry.h`) fed at `:1381-1418` |

To ask "is there a POI at map position P": one must invert `MapImageToScreen` for every candidate and call `Hit`, or project all markers and test the pixel radius (`radius + 3`, `:1457`) / `radius + 4` for hover (`:1435`). A map-space/nearest-in-ROC lookup does not exist today. **UNKNOWN:** there is no `DrawItemBase` API for "point at map coordinate".

---

## 7. Map filtering (which categories/icons are displayed)

### 7.1 The real mechanism: an inclusion registry, no exclusion set

* Storage: `static SceneItemStore<ItemsDatas> selectedItems;` — `DrawItemBase.cpp:41`; scene keyed; `GetSceneItemsSnapshot(sceneId)` — `:231-233` (header `DrawItemBase.h:86`).
* Populate: `DrawItemBase::AddItemDataFromJson(itemId)` — `:294-337` reads the category JSON (`itemsJsonData_<Scene>`, `:78-85`), converts coords, and `selectedItems.Add(sceneId, ItemsDatas(nameId, ...))` **plus `++markerFilterRevision`** (`:325-327`).
* Remove: `DrawItemBase::ClearItemData(itemId)` — `:339-343`, `selectedItems.Remove(itemId); ++markerFilterRevision;`.
* External entry points: the **`setItems` pipe command** — `CoreHostMain.cpp:674-684`:
  ```cpp
  if (type == "setItems") {
      MapToolsBridge::Shared().InvalidateCanvas("筛选已变化，本次圈选已取消");
      if (command.contains("add"))    for (const auto& value : command.at("add"))    AddItem(value.get<std::string>().c_str());
      if (command.contains("remove")) for (const auto& value : command.at("remove")) ClearItem(value.get<std::string>().c_str());
  ```
  (`AddItem`/`ClearItem` are the free functions at `DLL_API.cpp:388-396`.) Produced by `IMao-WinUI/Services/CoreHostService.cs:345-373` (`SetItemEnabledAsync`, `SetItemsEnabledAsync`, `SetItemsAsync`, `SynchronizeFilterAsync`) from the WinUI selection state (`FilterSelectionService` `IMao-WinUI/Services/FilterSelectionService.cs:82`, `LocalItemFilter` `IMao-WinUI/Helpers/LocalItemFilter.cs:13-33`); full state is re-pushed on every (re)connect at `CoreHostService.cs:164-167`. Filter changes therefore also cancel an in-flight lasso gesture (`CoreHostMain.cpp:675`). `Main.cpp:15-49` holds a legacy hard-coded startup list.
* Consumers: `DrawItemOnGameMap::GetAndFilterItemsData` (`DrawItemOnGameMap.cpp:85-107`, storage fetched in `GetBasicDataBySenceId` `:78-83`) and the minimap equivalent (`DrawItemOnMinMap.cpp:146`).
* Revision counter propagates a "the visible marker set changed" signal to every consumer: `MarkerFilterRevision()` `:391`; used in `ItemMarkerFrame::filterRevision` (`MapData.h:60`), `DrawItemOnGameMap.cpp:79`, `DrawMarkerInteraction.cpp:173`, `:1341`, `:1401`, `GamepadCursorTargets` (`CoreHostMain.cpp:329`), `NearbySelection` validation (`DrawItemBase.cpp:412`, `:500`, `:523`).
* UI list of categories: `IMao-WinUI/Views/Controls/FilterControl.xaml.cs:47-48` (`catalog.Items`, `catalog.Categories`); tests in `Tests/ManagedRuntime/MapFilterCatalogTests.cs`.

### 7.2 `GetFilteredPoints` is *not* a category filter

`DrawItemBase::GetFilteredPoints(scene, nameId)` — `DrawItemBase.cpp:370-377`:
```cpp
if (IsRefreshablePoint(nameId))
    return farmStore ? farmStore->CompletedIds(MarkerSceneState(scene)) : vector<string>{};
return markerStore->CompletedIds(scene, nameId);
```
Its name is misleading: it returns the **completed point-id list** for the category, which `GetAndFilterItemsData` uses to set `ItemDatas::isSaved` (`DrawItemOnGameMap.cpp:88-101`) — i.e. it decides *how* a marker is drawn (dimmed/coloured), not *whether* the category exists. (This naming trap is also flagged in `Docs/FarmMode_20260927.md:218`.)

### 7.3 Other per-point visibility gates

* Completed markers are hidden unless `showCompleted` or planning is on: `DrawMarkerInteraction.cpp:1364` `if ((planning.enabled || !showCompleted) && DrawItemBase::IsPointCompleted(...)) continue;`
* Layered-map role hides/redirects markers: `LayeredMap::RoleFor(item)` — `:1233-1236`, `:1367-1368` (`Hidden` is skipped so it is not clickable), direction badge at `:1281-1286`.
* Tools panel occlusion excludes both drawing and clicking: `ToolsPanel(...)` `:141-151`, applied at `:1371` (layout) and via `planningPanel.Contains(...)` in `MouseProcedure` (`:450`).

### 7.4 Could a route request "only show these categories"?

Yes — the mechanism already exists, and it is the same one the filter page uses:
* clear everything with `DrawItemBase::ClearItemData(<nameId>)` (or `DLL_API.cpp:393 ClearItem`),
* add the desired ones with `DrawItemBase::AddItemDataFromJson(<nameId>)` (`DLL_API.cpp:388 AddItem`),
* every consumer refreshes because `++markerFilterRevision` invalidates `ItemMarkerFrame::filterRevision` checks (`DrawMarkerInteraction.cpp:1341` inside `PlanningBinding::valid`, `:173` inside `SamePlanningView`).

Caveats for a feature author:
* `AddItemDataFromJson` **appends** and does not de-duplicate (`Docs/ProjectAudit_20260907.md:46-47`); a route that adds a category twice will produce duplicate markers.
* There is no "route wants this category" flag and no per-route filter state anywhere in `AutoRoute::Plan` — a filter request would be new state.
* The route-planning catalog (`RoutePlanningService.cpp:359-377`) reads the raw `itemsJsonData_*` arrays directly, **not** `selectedItems`; so planning still sees points whose category is currently filtered out. `addVisible` (`:613`) and the candidate scan (`:1318-1323`) work off the frames, which *are* filtered — so the two views can legitimately disagree.

---

## 8. Diagnostics / logging conventions in this area

### 8.1 The two channels

1. **Persistent structured log** — `StructuredLogger::Record(severity, category, message, details)` (`Runtime/StructuredLogger.cpp:103-125`).
   * File: `%LOCALAPPDATA%\IMao-WinUI\Logs\events-<YYYYMMDD>.jsonl`, one JSON object per line with `timestamp/severity/category/message/details` (`:111-117`).
   * Retention: 24×7 days and 100 MiB total, pruned on every record (`:20-21`, `PruneLocked` `:65-89`).
   * Severities seen in this area: `"info"`, `"error"`, `"warn"`. Categories seen: `"routes"`, `"markers"`, `"farm"`, `"gamepad"`, `"core"`, `"ipc"`.
   * Optional in-process observer: `SetObserver` (`:142-145`) — used by the managed host to mirror events; not used by route code.
2. **Diagnostics ring/stream** — `Diagnostics::Record(tag, details)` (`Diagnostics/Diagnostics.cpp`), used for per-frame / high-rate evidence (`overlay-frame`, `map-marker-sample`, `marker-filter`, `minimap-marker-sample`, `notification-*`). Examples: `DrawItemOnGameMap.cpp:55`, `:61`; `DrawItemOnMinMap.cpp:96`, `:113`; `ImGuiOverWindows.cpp:153`.

### 8.2 Every existing route-related log call

| call | location |
|---|---|
| `Record("info","routes","auto-route-solved", "targets=… elapsedMs=… initialLength=… planarLength=…")` | `RoutePlanningService.cpp:245-247` |
| `Record("info","routes","auto-route-replanned", "outcome=… targets=… oldLength=… newLength=… elapsedMs=…")` | `RoutePlanningService.cpp:349-351` |
| `Record("error","routes","route-escape-input-unavailable", GetLastError())` | `DrawMarkerInteraction.cpp:1090` |
| `Record("error","markers","marker-input-unavailable", GetLastError())` | `DrawMarkerInteraction.cpp:1105` |
| `Record("error","markers","completion-save-failed", message)` | `DrawMarkerInteraction.cpp:1669` |
| `Record("info","gamepad","guide-route-fallback[-unavailable]", …)` | `DrawItemOnMinMap.cpp:188`, `:196` |

There are **zero** log calls in `LoadEditRouteData.cpp` (hand-drawn route) and **zero** in `RoutePlanStore.h`.

### 8.3 Where new route logs should go

* Solver lifecycle / state transitions: `RoutePlanningService.cpp:245` (solve finished) and `:349` (replan outcome) are the established pair; a new "command applied/rejected" log belongs in `Command` right before the `return` at `:710` / in the `catch` at `:711`. Note that today command rejections are **not** logged at all — they only travel to the UI as `message`.
* Persistence failures: `RoutePlanStore.h:104`, `:108` throw with `GetLastError()` text but never log; `Worker`/`AutoWorker` catch and surface via `r.message` (`:249`, `:355`).
* Input/hook failures: follow `DrawMarkerInteraction.cpp:1090` / `:1105` style (severity `"error"`, category matching the subsystem, `GetLastError()` as `details`).
* High-rate/visual evidence: use `Diagnostics::Record` with a hyphenated `tag` and `key=value` space-separated `details`, as in `DrawItemOnGameMap.cpp:55-64`.

---

## 9. C++ tests for route planning

### 9.1 Files and targets

| file | target | CMake | runs by default? |
|---|---|---|---|
| `IMao-Core/tests/RoutePlanningTests.cpp` (959 lines) | `IMaoRoutePlanningTests` | `CMakeLists.txt:380-385`; `add_test` at `:385`; output to `x64/$<CONFIG>` (`:384`) | yes, part of CTest |
| `IMao-Core/tests/RoutePlanningServiceTests.cpp` (539 lines) + `RoutePlanningServiceTestHost.h` | `IMaoRoutePlanningServiceTests` | `CMakeLists.txt:389-398`, **`EXCLUDE_FROM_ALL`**, compiles the real `RoutePlanningService.cpp` with `-DIMAO_ROUTE_SERVICE_TEST` (`:392`), output `out/auto-replan-native/` (`:398`) | **no** — must be built explicitly; not registered with `add_test` |
| `IMao-Core/tests/RouteGamepadTests.h` (14.8 KB) | consumed by another harness (**UNKNOWN** which target — grep finds no `add_executable` referencing it) | – | – |

The test-only host replaces `StructuredLogger` and `DrawItemBase` with in-memory fakes — `IMao-Core/tests/RoutePlanningServiceTestHost.h:10-50` (note `IsPointCompleted` `:27`, `IsRefreshablePoint(Id)` `:28-29`, `HandleMarkerCommand` `:31-48`, `ApplicationDataDirectory()` → `root` `:12`). The switch is `RoutePlanningService.cpp:4-9`.

### 9.2 Test-name pattern

Plain hand-rolled harness, **no framework**: a file-local `int failures` plus `Expect(bool, message)` / `Check(bool, message)` and a `main` that calls a fixed list of suites.

* `RoutePlanningTests.cpp:19-21` (`Expect`), `:953-959`:
  ```cpp
  int main() {
      try { SolverTests(); GeometryTests(); StoreTests(); EscapeOwnershipTests(); DrawingVisibilityTests();
            AutoReplanTests(); FarmModeTests(); HotkeyPressOwnershipTests(); GuideHotkeyRoutingTests();
            HotkeyConfigurationTests(); GuidePaginationTests(); MarkerGuideProtocolTests(); Benchmark(); }
  ```
  Suite list with line numbers: `SolverTests` `:59`, `GeometryTests` `:110`, `StoreTests` `:149`, `EscapeOwnershipTests` `:301`, `DrawingVisibilityTests` `:321`, `HotkeyPressOwnershipTests` `:344`, `GuideHotkeyRoutingTests` `:415`, `HotkeyConfigurationTests` `:551`, `GuidePaginationTests` `:639`, `MarkerGuideProtocolTests` `:721`, `AutoReplanTests` `:789`, `Benchmark` `:867`, `FarmModeTests` `:887`.
* `RoutePlanningServiceTests.cpp:16` (`Check`), helpers `Command` `:17-18`, `Complete` `:19`, `Observe` `:21-30`, `Pump` `:31-35`, `Prepare` `:36-55`, `VerifyViewportSelection` `:64+`.

Each assertion is a sentence-style string, e.g. `RoutePlanningTests.cpp:190` `"store roundtrip preserves plan identity, order, skips, undo history and the farming setting"`.

### 9.3 How to run just these

* Default ctest target (documented invocation, `Docs/AutoRoutePlanningTests_20260908.md:9` shows the expected exe path): run `x64\<Config>\IMaoRoutePlanningTests.exe` directly (path fixed by `CMakeLists.txt:384`), or `ctest -R IMaoRoutePlanningTests`. Because the harness is a single `main`, selecting an individual suite requires temporarily editing the call list at `RoutePlanningTests.cpp:953-959` — there is no CLI filter (**UNKNOWN**: no `--filter` handling anywhere in the file).
* Service test: build the excluded target explicitly, e.g. `cmake --build <build-dir> --target IMaoRoutePlanningServiceTests --config Release`, then run `out\auto-replan-native\IMaoRoutePlanningServiceTests.exe`.
* Prebuilt artifacts from a previous build already exist in the tree: `out/build/windows-x64-release/CMakeFiles/IMaoRoutePlanningTests.dir/...` and `.../IMaoRoutePlanningServiceTests.dir/...` (object files only).

### 9.4 Coverage gaps relevant to a new feature

`RoutePlanningServiceTests.cpp` is the only place that exercises `RoutePlanningService::Command`, the worker threads, `ObserveMap` and real atomic persistence. **Not covered by any C++ test** (grep-verified):
* the drawing code (`DrawRouteOnMap.cpp`, `DrawRouteOnMinMap.cpp`, the `DrawMarkerInteraction.cpp` planning branch, `App.cpp:2681-2722` route assembly) — the closest C++ coverage is `DrawingVisibilityTests` (`RoutePlanningTests.cpp:321-343`) which only tests `DrawVisibility::Allows`;
* the hand-drawn feature end-to-end (`LoadEditRouteData`) — no test file references it;
* `Save`/`Delete` through the *service* (the store itself is well covered by `StoreTests`);
* managed/protocol coverage exists instead: `Tests/ManagedRuntime/RoutePlanningTests.cs:11-42` (state parsing incl. opaque ids) and `RunIpcAsync` (spawns the real host; invoked from `Tests/ManagedRuntime/Program.cs:171` when a program directory argument is supplied, `:222` `IMao-CoreHost.exe --pipe <name>`). Guide-window route commands are tested with a fake core in `Tests/GuideWindowRuntime/GuideWindowTests.cs:43-72`, `RouteControllerTests.cs`.

---

## 10. Explicitly undetermined

1. Which CMake target consumes `IMao-Core/tests/RouteGamepadTests.h` — no `add_executable` or `#include` of it exists in the repo (only the file itself).
2. Whether any shipped build configuration enables `IMAO_ROUTE_SERVICE_TEST` in production — the comment at `CMakeLists.txt:387-388` says it is test-only and never enabled on the production host, and `RoutePlanningService.cpp:4` is the only `#ifdef`; I did not inspect every preset. `CMakePresets.json` was not read.
3. Exact interpolation rule of `GenerateEquidistantPoints` (declared in `IMao-Core/src/util.h`, not read here) — only the call `GenerateEquidistantPoints(a, b, 5)` (`LoadEditRouteData.cpp:72`, `:183`) was confirmed.
4. Whether a route-driven category filter was ever prototyped — no code path, flag, or field exists in `AutoRoute::Plan`/`RoutePlanningView`, and no doc reference was found.
5. Precise semantics of `RouteDatas::name` for hand-drawn routes (it is always the literal string `"name"` at `LoadEditRouteData.cpp:132`, `:28`).
