# 路线库（保存/加载/切换/筛选）与手绘路线重做 —— 实施计划

> 状态：**实施中**。分支 `feature/route-list-handdrawn`，从 `main` @ `c9a4ef5` 开出。
> 本文是规格 + 实施计划。需求以**用户本次原话**为准（§1），代码事实以 §2 的文件:行号为准。
> **实施进度与实测结果见 §10**（每完成一个阶段就续写那一节）。
>
> **§9 三个问题已拍板（2026-09-28）**：
> Q1 手绘结束 = **工具栏按钮【完成手绘】**；Q2 手绘遇到正在导航 = **只提示、结果只保存不接管导航**；
> Q3 按路线筛选 = **默认开、记在路线自己身上**。
>
> **§3.0 已拍板（2026-09-28）**：手绘路线**不修补、整体重做**——旧实现（`LoadEditRouteData` /
> 旧 `SavedRoutes\*.json` / 渲染里的 `!automatic` 红色分支）**全部退役**，手绘变成"由同一个
> `RoutePlanStore` 存、走同一个投影与绘制路径"的一种路线。用户授权原话：
> *"手绘路线其实是老项目的遗留。如果你觉得重做这个功能对于以后这个项目的长久发展更好，
> 那你也可以重做这个功能。"*
>
> 导航接口（27 个 action、落盘 schema、命中/筛选机制）另有一份逐行证据地图：
> `Docs/NativeRoutePlanningMap_20260928.md`（542 行，由只读勘察产出）。**§2.7 是它与本文冲突/补充的条目，
> 优先级高于本文其余部分**（本文早期草稿在其中三处与代码不符，已按它改正）。

---

## 一、需求（用户原话）

### 1.1 路线列表（三条）

> "目前IMAO在游戏内大地图工具栏的路线规划工具栏里，没有保存路线和加载路线功能，
> 我希望在这里加一个路线列表，点击以后可以打开路线列表界面，可以是稍微大一点的弹窗，
> 里面展示保存的路线，每一行都包含了路线的追踪列表（创建路线时选择的那些点位的具体种类，
> 比如敌人叮叮咚就显示为"叮叮咚的图标 叮叮咚"这样），路线包含点位个数。
> 在这个路线列表界面最上方也可以看到当前的路线，可以选择保存，也可以选择删除（如果已经保存过的话）。
> 玩家可以选择已有的路线进行切换。"

### 1.2 手绘路线

> "当前手绘路线功能太过粗糙。请你将手绘路线的路线样式改成跟我们路线规划的路线样式一样。
> 手绘路线时，改成按一下Q，然后鼠标点击一下大地图上的某个位置，再按一下Q，点击下一个位置，
> 如此反复，默认第一个位置是起点，最后一个位置是终点。
> 如果点击的是目前已有的点位，则路线连到这个点位上，如果点击的位置没有任何点位，
> 则生成一个小圈里面显示数字，表示这是此路线第几个标记。"

### 1.3 统一存储

> "手绘路线和自动规划路线保存在一起。手绘路线和自动规划的路线唯一区别就是，
> 手绘路线可以选择IMAO里已有的点位（同时也是上游的库街区已有的点位），
> 也可以点击空白的地方创建IMAO没有的点位，这个创建的点位只存在于此路线中。"

### 1.4 应用路线时的筛选

> "应用路线以后，如果路线对应的点位有对应的图标，则大地图上也相应地筛选对应的点位和图标进行展示。"

---

## 二、已核实的事实（本对话读码/实测，不是转抄）

### 2.1 入口：游戏内工具栏到底在哪

| 事实 | 证据 |
|---|---|
| 游戏内大地图的路线工具栏 = **WinUI 工具窗** `MapToolsWindow.RenderRoute` 产出的按钮组 | `IMao-WinUI/Views/MapToolsWindow.cs:173-229`（`RenderRoute`，按钮条目从 `entries` 生成） |
| Shell 侧命令分发 | `MapToolsWindow` 的按钮 → `Func<string,Task> command` → `MapToolsController.RunCommandAsync`（`IMao-WinUI/Services/MapToolsController.cs:319-403`）→ `CoreHostService.ExecuteRoutePlanningAsync`（`CoreHostService.cs:410`）→ 核心 IPC `routePlanning` → `RoutePlanningService::Command`（`IMao-Core/src/Runtime/RoutePlanningService.cpp:583`） |
| 原生 ImGui 那套工具栏（`BuildPlanningToolbar`）**是死代码**，全项目无调用者 | `IMao-Core/src/ImguiDraw/Items/DrawMarkerInteraction.cpp:601-606` 的注释 + `:1197-1200`（`DrawMapToolsLauncher` 在已注册时直接 `return`；注释 "The WinUI window is the only expanded UI."） |
| 窗口页只有 `home` / `route` / `filter`，且原生侧对页名白名单校验 | `MapToolsWindow.ShowPage`（`:128-141`）；`IMao-Core/src/Runtime/MapToolsBridge.h:104`（`page != "home" && page != "route" && page != "filter"` 则拒绝） |
| 工具窗尺寸由 `LayoutForGame` 计算：route 页固定高 **370**、宽 ≤ 1000 | `MapToolsWindow.cs:304-320` |

**结论**：路线列表要做成"稍微大一点的弹窗"，最省的路径是**在同一个 `MapToolsWindow` 里加一个新页**（`routes`），并把 `LayoutForGame` 的尺寸按页给值（例如 profile 页 1000×620）。不改 `MapToolsBridge` 的白名单就一定会被原生拒绝，这一行**必须**同步改。

### 2.2 路线数据模型与存储

| 事实 | 证据 |
|---|---|
| 一条已保存路线 = `AutoRoute::Plan{id,name,profileId,sceneId,start,stops,skipped,skipHistory,farmMode}` | `IMao-Core/src/Runtime/RoutePlanningModel.h:24-36` |
| 一个目标 = `ItemDatas{itemId,nameId,screenCoordiante,itemMapROC,isSaved,layer{stateId,countryId,floorId,level}}` | `IMao-Core/src/Domain/MapData.h:8-23` |
| **`nameId` 就是"点位种类"**（= 上游 items 数据里的叶子类型 id，也是筛选项 id、图标 id） | 构建处 `RoutePlanningService.cpp:359-377`（`nameId=category.value("id")`）；展示名 `r.names[nameId]=category.value("name")`（`:366`）；托管侧同一身份链：`StringItems.cs:208`（`ItemDatas(item.Name, translation)`）→ `MapFilterCatalog.cs:179-184`（`source.Id` + `icons.GetValueOrDefault(source.Id)`） |
| 落盘：`%LOCALAPPDATA%\IMao-WinUI\SavedRoutes\Auto\<profile>\<uuid>.json` + `active.json` | `RoutePlanningService.cpp:382`；`RoutePlanStore.h:32-47,75-90,132-138` |
| 动作集合：`new end tool setStart add addVisible toggle remove undo clear generate replan activate stop delete pause resume skip undoSkip farm guide complete save load list state` | `RoutePlanningService.cpp:591-709` |
| **`save` / `load` / `delete` / `list` 已经存在，并且桌面端 `FunctionPage` 已在用** | `RoutePlanningService.cpp:694-708`；`IMao-WinUI/Views/FunctionPage.xaml.cs:245-294` |
| `load` 会写 `active.json`、重置导航、但不自动开始（提示"路线已加载，点击继续导航"） | `RoutePlanningService.cpp:701-707` |
| `list` 返回 `[{id,name,sceneId,sceneName}]`（**没有点位个数、没有类型列表**） | `RoutePlanStore.h:116-128` |
| `Load` **强校验每个 stop 都能在本地点位目录里解析**，且 nameId / 坐标必须逐字一致（容差 1e-6） | `RoutePlanStore.h:59-69` |
| `Save/Validate` 要求 `stops` 非空、`start.valid`、`start.sceneId==sceneId`、每个 stop 的 `layer.stateId==scene.kuroStateId` | `RoutePlanStore.h:144-157` |
| 托管侧已有 `SavedAutomaticRoute{Id,Name,SceneId,SceneName}` 与 `SavedRoutes` 快照字段 | `IMao-WinUI/Models/RoutePlanningState.cs:119-126,37` |

### 2.3 手绘路线现状

| 事实 | 证据 |
|---|---|
| 现实现：Q 按一下记 A、再按一下记 B 并**立刻落盘一条线段**，线段两点之间插值 5 个采样点 | `IMao-Core/src/ImguiDraw/Routes/LoadEditRouteData.cpp:86-141`（`Thread_KeyMonitoring_AddRouteDatas_ByMousePos`）、`:71-78`（`AddRouteDatas` + `GenerateEquidistantPoints`）、`:195-214`（`WriteRoutesDatas`） |
| 落盘位置与格式**完全独立**：`SavedRoutes/<routeJsonName>.json`，内容是 `{场景名: [[[x,y],[x,y]], …]}`，只有裸坐标 | `LoadEditRouteData.cpp:24-42,195-214`；默认名 `"Routes"`（`:22`），改名入口 `SetRouteJsonName`（`:80-84`）← `coreHost.SetRouteNameAsync` ← `FunctionPage.xaml.cs:95-99` |
| 渲染：**和自动路线共用** `DrawRouteOnMap`，但自动路线走彩色、手绘走**纯红**且更细（1.5 vs 2.0/3.5） | `IMao-Core/src/ImguiDraw/Routes/DrawRouteOnMap.cpp:32-71`（颜色/粗细在 `:58-60`） |
| 手绘（`automatic=false`）在 `DrawVisibility::Allows` 里**无条件通过** | `RoutePlanningModel.h:47-53` |
| Q 轮询与规划模式互斥（"Route planning owns its own draft"） | `LoadEditRouteData.cpp:105-111` |
| 坐标来源：`App::TryGetRoutePoint(ROC, sceneId)`（把屏幕光标换算成**场景相对 ROC**） | `IMao-Core/src/App/App.cpp:2504-2521`；另有 `GetMapCoordinatesOfMousePos`（`:2496-2502`） |
| 路线目标徽标（圈+数字）**已经存在**：plan 的 stop 画成小圆 + 序号，当前目标用橙色 | `DrawMarkerInteraction.cpp:1563-1583` |

### 2.4 地图输入与点位命中

| 事实 | 证据 |
|---|---|
| 鼠标点击由**底层鼠标钩子**捕获，再按"当帧登记的命中区域"派发，与窗口焦点无关（`focused = gameFocused \|\| ToolsCanvasFocused()`） | `DrawMarkerInteraction.cpp:418-534`（钩子）、`:584-589`（`AddRegion`）、`:1197-1224`（launcher 就是"登记一个区域"的现成范例） |
| 已有点位的命中目标形如 `p:<pointId>` / `g:<groupId>`，点击派发到 `clicks` 队列 | `:1457`、`:1520`（`AddRegion(..., "p:"+id)`）、`:1646-1671`（消费） |
| 规划模式下的选点点击 -> `RoutePlanningService::TogglePoint` / `AddPoints` | `:1652-1664`、`:1620-1625` |
| 已有可复用的"地图是否新鲜"判据 | `planningBinding.enabled && planningBinding.valid && planningBinding.presented.Fresh()`（`:454`、`:510`） |
| 框选/套索/指定起点的后台手势已支持"在游戏内直接拖" | `:468-487`（`backgroundGesture`） |

### 2.5 筛选机制

| 事实 | 证据 |
|---|---|
| 筛选是**按点位类型 id 逐个开关**，进程内单一期望状态 + 落盘 + IPC 同步 | `IMao-WinUI/Services/FilterSelectionService.cs:105-132`（`SetEnabled(IEnumerable<string> itemIds, bool enabled)`） |
| 类型 id 与路线 stop 的 `nameId` **同一个身份** | 见 2.2 第 3 行；`MapFilterCatalog.Load` 的输入 `MapFilterSourceItem(Id, Name, LegacyCategory)` 就是 `StringItem.itemDatas`（`FilterSelectionService.cs:80-81`） |
| 每个类型有图标路径（`MapFilterItem.IconPath`，来自 `icon-manifest.json`） | `MapFilterCatalog.cs:16-24`、`:227-254` |
| 工具栏"点位筛选"页用的是同一个服务 | `IMao-WinUI/Views/Controls/FilterControl.xaml.cs:147-190`、`MapToolsWindow.cs:80,136` |

**结论**：第 1.4 条（应用路线后筛选对应点位/图标）可以**直接复用** `FilterSelectionService.SetEnabled`，类型 id 由路线的 `nameId` 集合给出，图标由 `MapFilterCatalog` 按同一 id 提供 —— 不需要新造身份映射。

### 2.6 测试与验证入口

| 事实 | 证据 |
|---|---|
| 核心服务测试：`IMaoRoutePlanningServiceTests`（独立 harness，不碰真 IPC） | `IMao-Core/tests/RoutePlanningServiceTests.cpp`、`RoutePlanningServiceTestHost.h`；跑法见 `scripts/Test-Runtime.ps1:41-43` |
| 纯模型测试：`IMaoRoutePlanningTests` | `IMao-Core/tests/RoutePlanningTests.cpp` |
| 托管侧路线测试：`Tests/ManagedRuntime/RoutePlanningTests.cs`（真的读写 `SavedRoutes\Auto\<profile>`） | `:218,371,434` |
| 工具窗集成测试：`Tests/MapToolsRuntime/App.xaml.cs`（用真 `MapToolsController`） | `:61,241,262` |
| 全量闸门 | `scripts/Test-Runtime.ps1`（exit 0 才算过） |

### 2.7 勘察补充与**对本文早期草稿的更正**（来自 `Docs/NativeRoutePlanningMap_20260928.md`）

这七条改变了实施细节，**优先级高于本文其它小节**：

| # | 更正 / 补充 | 影响 |
|---|---|---|
| C1 | **`rename` 不需要新动作**：`save` 带 `name` 就是"原地改名"（id / 文件名不变） | 删掉 3.3 的 `rename` 行；列表页的"保存/改名"直接走 `save` + `name` |
| C2 | ⚠️ **不存在"地图空间里 P 点有没有 POI"的查询**。`Hit(x,y)` 只按**屏幕像素**查当帧登记的区域；`GetFilteredPoints` **不是分类筛选**，它返回的是**已完成台账**（`DrawItemBase.cpp:370-377`） | 3.5 的"点到已有点位就连它"**必须改成显式两步**（见下） |
| C3 | 筛选的**真正机制**是 `selectedItems` 包含注册表（`DrawItemBase.cpp:41`），由管道命令 **`setItems`** 驱动（`CoreHostMain.cpp:674-684` → `AddItem`/`ClearItem`），托管侧出口是 `CoreHostService.cs:345-373` / `FilterSelectionService` | 3.6 走托管侧 `FilterSelectionService.SetEnabled` 即可（它就是 `setItems` 的生产者），**但**要绕开两个坑（见 C4、C5） |
| C4 | ⚠️ `AddItemDataFromJson` **只追加、不去重**（`Docs/ProjectAudit_20260907.md:46-47`） | 3.6 收敛筛选时**只发"要关掉的"**，再发"要打开的"，并保证幂等；不要重复全量重推同一批 id |
| C5 | ⚠️ **规划目录读的是原始 `itemsJsonData_*`，与筛选无关**：`addVisible` / 视野候选 / 选点**仍然看得见被筛掉的点** | 3.6 是**纯显示层**收敛，不改变规划行为 —— 这是好事（"应用路线"不该让选点少东西），**写进提示语与文档**，别让人以为是 bug |
| C6 | `IMaoRoutePlanningServiceTests` 是 **`EXCLUDE_FROM_ALL`**，且要用 `-DIMAO_ROUTE_SERVICE_TEST` 编译真实服务（`CMakeLists.txt:389-398`，输出 `out\auto-replan-native\`） | 阶段 1/2 的验收命令要**显式构建该目标**，不能只跑 `ctest` |
| C7 | **命令被拒绝时从不写日志**（只有 `message` 回给 UI）；路线区总共只有 6 处 `StructuredLogger::Record`；`LoadEditRouteData.cpp` 与 `RoutePlanStore.h` 里 **0 处** | 新增动作必须在 `RoutePlanningService.cpp:710/:711` 一带补日志（尤其**拒绝原因**），否则真机排障只能靠猜 |

**C2 的具体落法（3.5 的实现口径）**：

```
点击(屏幕坐标) ──Hit(x,y)──> "p:<id>" / "g:<id>" ?
                              ├─ 是 → catalog 点：用该 id 在 catalog 里取 nameId/itemMapROC
                              └─ 否 → 需要"这一点附近有没有 POI"⇒ 只能自己算：
                                     遍历当帧 frame.markers（已投影到屏幕）
                                     取 screenCoordiante 与点击点的距离 ≤ radius+4
                                     半径口径必须与绘制/悬停一致（DrawMarkerInteraction.cpp:1435）
                                     命中多个 → 取最近（NearestRouteSnapPoint 已有纯函数可复用）
                                     全不命中 → free 点
```

**并集口径**：`Hit()` 的 key 与 `frame.markers` 的 `layer.stateId+itemId` 都等于 `AutoRoute::Key()`，所以两条路给出的身份是同一个，不会出现"连到了 A 却记成 B"。

---

## 三、目标设计

### 3.0 决策：手绘路线整体重做（含退役清单）

**结论：重做，而且不是"把旧代码改得好看点"，是把它从架构里拿掉。** 理由不是"旧代码丑"，而是它**与全项目的承重不变量方向相反**：

| 维度 | 新架构（自动路线） | 旧手绘实现 | 并存下去会怎样 |
|---|---|---|---|
| 数据单位 | 有序的**点位身份** `AutoRoute::Plan::stops`（`Key()` = `<stateId>:<itemId>`） | **裸线段坐标** `{场景: [[[x,y],[x,y]],…]}`（`LoadEditRouteData.cpp:195-214`） | 手绘永远拿不到"点位/类型/图标/完成状态/跳过/刷怪采集" |
| 落盘 | 每路线一文件 + `active.json`（`RoutePlanStore`） | **全项目共用两个文件**，名字来自 `routeJsonName`（默认 `"Routes"`），改名要单独一条 IPC | 两个路线系统永远无法互相理解 |
| 模型语义 | `Plan`（起点 + 有序点位） | 每个 Q 对 = 一条**独立线段**（两点之间插值 5 个采样点，`util.h`），磁盘上仅按场景分组，**内存里没有"一条路线"这个概念** | "路线列表"对手绘无从谈起 |
| 绘制 | `App.cpp:2681-2722` 由 `Plan` 投影出 `RouteDatas` 段，`automatic=true` | 走 `GetRoutePointsScreen` 旁路，并把 `RouteDatas.automatic` 留 false，于是绘制里有**两条**红色分支 | 用户要的"样式一样"，正确修法是**删掉分支**而不是让手绘去伪装成自动 |
| 可见性 | `DrawVisibility::Allows` 统一裁决（profile / activeId / previewId / orderRevision / 小地图只在导航时） | `Allows` 里第一行 `if(!route.automatic) return true;` ——**绕过全部裁决** | 换档案、退出导航、换地图后残留旧线都是这一类问题 |
| 可观测 | 结构化日志 + `routePlanningChanged` 快照 | `LoadEditRouteData.cpp` **0 处日志**、**0 个测试** | 出问题只能靠猜 |

**所以重做的核心动作是"让手绘变成一种 `Plan`"**，而不仅仅是"让手绘好看一点"。做完之后：

- 手绘路线自动获得：路线列表、切换、删除、改名、完成状态、跳过、刷怪采集、按路线筛选、`active.json` 恢复、结构化日志——**全部零新增代码**。
- 绘制层少一个分支：`RouteDatas.automatic` 这个字段在"全是自动/统一路线"之后可以彻底删掉（含 `DrawVisibility` 的特例与用例 341-342）。

#### 退役 / 改写 / 保留

| 对象 | 处置 | 依据（调用点） |
|---|---|---|
| `LoadEditRouteData`（类、线程、JSON 读写、`AddRouteDatas`、`WriteRoutesDatas`、`SetRouteJsonName`、`ReadRoutesJson`、`LoadRoutesDatasFromLocal`、`GetRoutesSnapshot`） | **删除** | 调用点仅 `DLL_API.cpp:13,58,87,277,433,437,441`、`Main.cpp:9,61,71`、`CoreHostMain.cpp:737-751`、两个 Draw 的 `GetRoutePointsScreen` |
| `DrawRouteOnMap::GetRoutePointsScreen` | **删除** | 唯一调用者 `App.cpp:369`（旧手绘投影） |
| `DrawRouteOnMinMap::GetRoutePointsScreen` | **删除** | 唯一调用者 `App.cpp:527` |
| `DrawRouteOnMap` / `DrawRouteOnMinMap` 的两个 `#include "LoadEditRouteData.h"` | **删除**（解耦：绘制不再认识任何存储） | `DrawRouteOnMap.cpp:2` 附近、`DrawRouteOnMinMap.cpp:2` |
| 绘制里的 `!routeDatas.automatic` 颜色/粗细分支、`automatic &&` 虚线条件、`DrawVisibility::Allows` 的手绘放行行 | **删除** | `DrawRouteOnMap.cpp:58-61`、`DrawRouteOnMinMap.cpp:58-61`、`RoutePlanningModel.h:48` |
| `RouteDatas::automatic` 字段 | **删除**（连同用例 341-342 的断言改写） | 仅剩的读取点：两个 Draw、`App.cpp:2698,2718`、`RoutePlanningTests.cpp:322-342,860` |
| `FrameState::mapRoutes/minimapRoutes` + `App.cpp:2678` 快照 | **保留**（这是绘制层的 DTO 边界，本身没问题） | `FrameState.h:32` |
| `App.cpp:2681-2722` 的 `appendRoute` 投影 | **保留并扩展**：手绘路线也走它（`Plan.handDrawn` 只影响列表标签，不影响样式） | `App.cpp:2681-2705` |
| Q 轮询线程（按下沿 + 光标 + 场景） | **移到新手绘模块**（`HandDrawnInput`），输入源不变，语义改"记一个点" | `LoadEditRouteData.cpp:86-141` |
| `CoreHostService.SetRouteNameAsync/LoadRoutesAsync/LoadRouteAsync` + `CoreHostMain.cpp:737-751` + `FunctionPage` 的路线名输入框/打开目录/加载按钮 | **删除**（新列表页取代；"打开目录"在设置页已有 `SettingsPage.xaml.cs:1515`） | `CoreHostService.cs:403-408`、`CoreHostMain.cpp:737-751`、`FunctionPage.xaml.cs:95-125`、`FunctionPage.xaml:64-72`、测试替身 `FakeServices.cs:85-87` |
| `Tests/ManagedRuntime/Program.cs:188-200` 的 `setRouteName`/`loadRoute` 用例 | **改写**为"路径穿越/坏文件被拒"的新入口用例 | `Program.cs:188-200` |
| 旧 `SavedRoutes\*.json` 数据 | **一次性导入后再封存**（见 §3.2.1），**绝不静默丢弃** | — |

> ⚠️ 本项目有条现成的教训（`MEMORY.md` §6 K 系列）：**"看起来像工具栏"和"真的被画出来"是两件事**。
> 同款纪律用在这里：删 `LoadEditRouteData` 之前，必须把上表的调用点**逐个**改掉并让编译器证明没有漏网
> （删头文件、让编译失败是最可靠的检查）。

### 3.1 数据模型：`Plan` 支持"自由点"

新增 stop 种类，**默认值必须等价于今天的行为**（老文件缺字段 = catalog 点）：

```cpp
// RoutePlanningModel.h
enum class StopKind { Catalog, Free };   // 序列化 "kind":"catalog" | "free"，缺省 catalog
```

`ItemDatas` 不动（它是点位资源的通用结构）。自由点这样填：

| 字段 | 自由点取值 |
|---|---|
| `itemId` | `"free:<n>"`（n = 在**本路线内**从 1 开始的稳定序号，用于 key 与去重） |
| `nameId` | 空字符串（**没有类型 ⇒ 没有图标、不参与筛选**） |
| `itemMapROC` | 该次点击换算出的场景相对 ROC（`App::TryGetRoutePoint` 的返回值） |
| `layer.stateId` | 该场景的 `kuroStateId`（与已有点一致） |
| `layer.countryId/floorId/level` | 0 / 空 / 空 |

落盘时每个 stop 多写一个 `"kind"`：

```json
{"stateId":8,"pointId":"free:2","nameId":"","x":123.4,"y":-56.7,
 "countryId":0,"floorId":"","level":"","skipped":false,"kind":"free"}
```

`RoutePlanStore` 的改动（**唯一必须放宽的地方**）：

- `Save`：catalog 点原样；free 点写 `kind:"free"` 且 `nameId` 允许为空。
- `Load`：读到 `kind=="free"` 时**不调 `resolve`**，只校验坐标有限、`stateId==scene.kuroStateId`；其余与今天一致（catalog 点仍然严校验，**不放松**）。
- `List`：多返回 `stopCount` 与 `kinds`（去重后的 `nameId` 列表，仅 catalog 点；供列表页显示"图标+名字"与"点位个数"）。
- `Delete`/`LoadActive`/`ClearActive` 不变。

> ⚠️ **不对自动路线放宽**：catalog 点的 nameId / 坐标逐字校验（`RoutePlanStore.h:64-66`）是"点位资源变了就报错"的承重逻辑，本次不动。

### 3.2 存储布局：手绘与自动路线同处

沿用**同一个 store**，只按性质分子目录，列表**合并展示**：

```
%LOCALAPPDATA%\IMao-WinUI\SavedRoutes\
  Auto\<profile>\<uuid>.json      ← 自动规划路线（今天就在这）
  Hand\<profile>\<uuid>.json      ← 手绘路线（同 schema，含 kind:"free" / "handDrawn":true）
  Legacy\<imported-*.json>        ← 导入后的旧文件封存地（不删，见 §3.2.1）
```

- 两条子目录都用 `AutoRoute::RoutePlanStore`（同一份 Save/Load/Delete/List 代码），`Plan` 里新增 `bool handDrawn = false;`（落盘 `"handDrawn":true`）作为"来源"标记（只影响列表标签，不影响样式与行为）。
- `List` 合并两边，返回项多一个 `handDrawn` 字段。
- `Auto`/`Hand`/`Legacy` 目录名进 `ProgramPackageValidation` 的保留目录清单是**不必**的（`SavedRoutes` 已经在里面，见 `IMao-WinUI.Core/Updates/ProgramPackageValidation.cs:29`）。

#### 3.2.1 旧数据：一次性导入，然后封存（**不丢、不造假**）

旧文件是 `{场景名: [[[x,y],[x,y]], …]}`，只有坐标，**没有点位身份**。所以：

- 导入规则（"保真"优先）：每条手绘线段 → 一个 **free 点对**；端点若**恰好**能与本地 catalog 里的点匹配（同一场景、坐标差 ≤ 1e-6），
  则升级为 **catalog 点**（这样"当时画在叮叮咚上"的线能恢复出类型与图标）；匹配不上就留 free 点。
- 导入单位：**一个旧文件 = 一条手绘路线**（`handDrawn=true`，名字取文件名，场景取文件内出现最多的场景；跨场景的旧文件按场景**拆成多条**并加后缀）。
- 落点：`Hand\<profile>\<uuid>.json`；原文件**移动到 `Legacy\`**（不是删除），并记录 `imported-<原文件名>.json`。
  这样"导入是有记录的、玩家能在设置页的『打开路线目录』里看到它还在"。
- 幂等：`Legacy\` 里的文件不再参与导入；导入器只看 `SavedRoutes\*.json`（顶层）。导入失败（解析不过、坐标非有限、线段过长）→
  **跳过该文件并写日志**，绝不因为一个坏文件挡住其余数据。
- 时机：核心启动时（`LoadEditRouteData::Initi` 今天的位置，`Main.cpp:61` / `DLL_API.cpp:87`）跑一次；
  只在"顶层还有旧文件"时工作，之后是空操作。
- 验收：用真实旧文件（本机 `%LOCALAPPDATA%\IMao-WinUI\SavedRoutes\*.json`）跑一次导入，
  断言"路线条数 = 旧文件数（或拆分后条数）、点数 = 端点数、`Legacy\` 里逐字节等于原文件"。

> 这条替代了初稿的"旧格式只读保留、不迁移"。**只读保留的问题是：旧渲染旁路必须留着**，
> 而那条旁路正是本次要拆掉的东西。导入则让旧旁路可以干净删除，同时数据一条不丢。


### 3.3 命令协议（新增/扩展，全部走 `RoutePlanningService::Command`）

| action | 参数 | 语义 | 备注 |
|---|---|---|---|
| `list`（扩展） | — | 返回合并列表，每项含 `id,name,sceneId,sceneName,handDrawn,stopCount,kinds[{nameId,name,icon?}]` | 图标不在核心侧解析，只给 `nameId`，托管侧用 `MapFilterCatalog` 查图标 |
| `switch`（新） | `routeId` | = `load` + 立刻恢复导航（`runRequested=true`） | 与 `load` 分开，保留 `load` 的"只加载不导航"语义（桌面端在用） |
| `current`（新） | — | 返回"当前路线"的完整描述（活动路线，或还没保存的草稿预览） | 列表页顶部那一行 |
| `save`（扩展） | `name` + `target` | 支持"保存当前草稿为手绘路线"（`target:"hand"`），写进 `Hand\<profile>`；**改名也走它**（现有行为就是带 `name` 原地改写，id/文件名不变） | 现在的 `save` 只懂 `preview` / `active`（`:694-700`） |
| `delete`（已有） | `routeId` | 删除（含正在导航的那条） | 不改 |
| 手绘（新，见 3.5） | `handStart` / `handPoint` / `handUndo` / `handCancel` / `handCommit` | 见 3.5 | |

⚠️ **`routeId` 前置校验会挡路**：`RoutePlanningService.cpp:592-593` 对任何带 `routeId` 的动作要求"必须等于当前活动路线"，`switch` 必须显式加入豁免名单（`delete` 已经在里面）。

另外按 C7：**`switch`/`save`/手绘的每一次拒绝都要落日志**（现在是静默返回 `message`），真机排障只靠这行。

### 3.4 交互：路线列表页（`routes` 页）

**入口**：`RenderRoute` 的按钮组里加两个按钮（"路线列表"总是可见；"保存当前路线"在没有可保存对象时置灰）：

```csharp
Add("routes", "路线列表", true, false);            // 打开列表页
Add("saveCurrent", "保存当前路线", canSave, false); // 草稿预览或活动路线
```

**页面内容**（新 `MapToolsWindow.ShowRoutes(RoutePlanningState)`，与 `ShowPage("routes")` 配套）：

```
┌ 路径自动规划 · 路线列表 ─────────────────────────────┐
│ 当前路线：<名字或"未保存的预览"> · N 个点 · [保存] [删除] │
│ 类型：[图标 叮叮咚]×3 [图标 宝箱]×1 …                   │
├──────────────────────────────────────────────────┤
│ ○ 一号路线     12 个点 · 今州 · 自动                    │
│   [图标 叮叮咚] [图标 宝箱] …                          │
│ ● 我的手绘     5 个点 · 拉海洛 · 手绘  (当前)            │
│   [图标 无] 自由点×2                                   │
│ ○ …                                              │
└──────────────────────────────────────────────────┘
```

- 行 = 一条路线；`○/●` 由 `AutoRoute::SameRouteId(id, activeId)` 决定。
- 每行显示：名字（空则 id）、`stopCount`、`sceneName`、来源标签（自动/手绘）、**去重后的类型徽标行**（图标 + 名字，取 `MapFilterCatalog`）。
- 顶部"当前路线"行：活动路线，或**还没保存的草稿**（`preview`）；有则亮起【保存】【删除】，没有则置灰。
- 点击行 = `switch`（应用该路线并开始指引）。
- 手柄：复用现成的 `navigation` 机制（按钮列表）→ 每行渲染成一个按钮，`RebuildNavigation` 自动把它纳入方向导航（`MapToolsWindow.cs:276-302`）。

**尺寸**：`LayoutForGame` 增加 `Page=="routes"` 分支（宽 1000、高约 620，仍受客户端高度约束），并同步 `ShowPage` 的白名单 + `MapToolsBridge.h:104` 的页名白名单。

**数据流**：进入页面时 `list` + `current`；`RoutePlanningChanged` 事件照旧驱动 `RenderRoute`/`RenderRoutes`（`MapToolsController.OnRouteChanged`，`:72`）。

### 3.5 交互：手绘路线（重做）

**状态机**（核心侧新增 `AutoRoute::HandDrawn` 状态，由 `RoutePlanningService` 持有）：

```
空闲 ──handStart(场景, 首点)──> 绘制中
绘制中 ──handPoint(点)──> 绘制中（追加，序号 +1）
绘制中 ──handUndo──> 绘制中（去掉最后一个点；空了就结束）
绘制中 ──handCancel──> 空闲（丢弃）
绘制中 ──handCommit(name)──> 空闲 + 写 Hand\<profile>\<uuid>.json + 列表刷新
```

**"按 Q 一次 = 记一个点"的判定**（关键，且**不需要**改 `TryGetRoutePoint`）：

现有 Q 轮询线程已经在做"按下沿 + 光标位置 + 场景"三件事（`LoadEditRouteData.cpp:86-141`）。**输入源不变，但这段代码要搬到新手绘模块**（`LoadEditRouteData` 整体退役，见 §3.0），线程体与 `TryGetRoutePoint` 的用法照搬：

- 把 `RoutePlanningService` 的规划模式判断（原 `:107`）扩展为"规划模式 **或** 手绘模式"。
- 手绘模式下，**每一次 fresh press**：
  - 若 `HandDrawn` 未开始：`handStart(scene, ROC)` —— 这就是起点；
  - 否则：`handPoint(ROC)` —— 追加下一个标记。
- 于是用户的手感正是"按一下 Q，点一下地图，再按一下 Q，点下一个位置"：Q 之后光标停在新位置，点下去落在新点。
- **结束**：工具栏按钮【完成手绘】（§9 Q1 已拍板）→ `handCommit`。

**"点到已有点位就连到它"**：手绘点收集时做一次命中判定（**口径见 §2.7 C2，这是唯一被勘察改写过的一处设计**）：

- 输入：点击时的屏幕坐标 + 当帧 `Hit(x,y)`（`DrawMarkerInteraction.cpp:259-267`，只查当帧登记区域）。
- **若命中 `p:<id>` / `g:<id>`**：生成 **catalog 点**（`itemId=id`，`nameId` 由 catalog 取，坐标取 `itemMapROC`）——即"连到这个点位"。
- **若没命中**：**不能**直接当自由点，还要再问一次"这一点附近有没有 POI"——因为 `Hit` 只覆盖当帧登记的区域，而**不存在**地图空间的 POI 查询（C2）。做法：遍历当帧 `frame.markers`（已投影到屏幕），取 `screenCoordiante` 与点击点距离 ≤ `radius + 4`（半径口径与绘制/悬停一致，`:1435`），命中多个取最近（复用现成纯函数 `NearestRouteSnapPoint`，`RoutePointSelectionInput.h:59-67`）。
- **两条路都不中**：才是 **free 点**（§3.1）。
- 两条路的身份都等于 `AutoRoute::Key()`，所以不会"连到了 A 却记成 B"。

**鼠标点击怎么到达核心**（这条是本次最大的技术风险，方案已定）：

- 底层鼠标钩子 `MouseProcedure` 已经在游戏聚焦时工作，且**只看"当帧登记过的命中区域"**。
- 手绘模式下，在**地图画布整块区域**登记一个 catch-all 区域（`AddRegion`，key = `"route:hand:canvas"`，参考 `DrawMapToolsLauncher` 的登记方式 `:1197-1224`）。
- 钩子只接受 `regionsFresh`（`regionsAt` 100ms 内）的命中 —— 即**渲染在跑**时才接受点击，天然避免了"截图卡住时点击落到错误位置"。
- 需要在 `MouseProcedure` 的 `backgroundGesture` 判定（`:468-475`）旁边加一条手绘分支：左键按下即把"屏幕坐标"投递给核心（`handPoint`），**不进入 drag 手势**，并 `return 1` 吃掉这次点击（不让游戏收到）。
- 记录时同样要求 `planningBinding.presented.Fresh()`（`:454` 同款判据），保证"点到的位置"与"屏幕显示的位置"是同一帧。

**渲染（"样式和路线规划一样"）**：

- 手绘路线的 `RouteDatas.automatic` 置 **true**（这样它就走自动路线的颜色/粗细/虚线逻辑，`DrawRouteOnMap.cpp:58-60`），同时 `handDrawn=true` 只用于**列表页标签**，不用于样式。
- 结果：手绘路线 = 自动路线的蓝色 2.0px 实线 / 预览虚线 / 当前段加粗 —— 与"路线规划的路线样式一样"。
- 自由点的"小圈 + 数字"：复用已存在的 stop 徽标画法（`DrawMarkerInteraction.cpp:1563-1583`），但**画在点位本身**（圆心 = 该 free 点的屏幕位置），而不是像 catalog 点那样偏到右侧 `radius+12` 处；数字 = 该点在路线中的序号。
- 起/终点：沿用现有"起"字标记（`:1552-1561`），终点在本次加一个对称的"终"（同款圆 + 字），因为用户明确说"默认第一个位置是起点，最后一个位置是终点"。
- 小地图：`DrawRouteOnMinMap` 走同一份 `DrawVisibility`，手绘路线同样可见（不动）。

**约束（写进提示，不静默处理）**：

- 一条手绘路线**只允许同一个场景**：换场景点击 -> 拒绝并提示"请在同一条路线上使用同一张地图"（`Plan.sceneId` 是单值，跨场景会破坏 `start.sceneId==sceneId` 与 `layer.stateId` 校验）。
- 手绘与自动选点互斥（沿用今天 Q 轮询里的那句互斥）。
- 手绘期间若有活动路线在导航：**只提示、不打断**，手绘提交后不自动切换导航（§9 待拍板）。

### 3.6 应用路线后的筛选（1.4）

**语义**：应用一条路线时，把筛选**收敛到这条路线涉及的点位类型**——"如果路线对应的点位有对应的图标，则大地图上也相应地筛选对应的点位和图标进行展示"。

**实现（按 §2.7 C3/C4/C5 校正后的口径）**：

1. 清单 = 该路线所有 catalog 点的 `nameId` 去重集合（free 点没有类型，不参与）。
2. **只走托管侧的 `FilterSelectionService.SetEnabled`**——它就是核心 `setItems` 管道的唯一生产者（`FilterSelectionService.cs:105-132` → `CoreHostService.cs:345-373` → `CoreHostMain.cpp:674-684`），所以**核心侧不需要为筛选加任何新代码**。
3. **只发差集，不重推全量**：先 `SetEnabled(要关掉的 ids, false)`，再 `SetEnabled(清单, true)`；`SetEnabled` 内部本身就只对"真正变化的 id"落盘+同步（`FilterSelectionService.cs:112`）。**不要**把全部 id 反复全量推送——`AddItemDataFromJson` 只追加不去重（C4）。
4. 图标随类型自动跟着走（`FilterControl` 用的是同一 id 的 `IconPath`），**不需要额外工作**。
5. 该行为做成路线列表页顶部的开关「按路线筛选点位类型」（默认开，符合用户原话）。开关状态记在**路线自己**身上（与 `farmMode` 同样的哲学：`Plan::filterByRoute`），落盘。
6. 关闭开关或切到别的路线时，**恢复**为"应用前记录下来的集合"（在 `MapToolsController` 里保存一份 `previousFilterSet`，仅内存，进程内有效）。

**边界**：

- 如果清单为空（全是 free 点）：不清筛选，只提示"该路线没有可筛选的点位类型"。
- ⚠️ **这是纯显示层收敛**：规划目录读的是原始 `itemsJsonData_*`，所以 `addVisible`、视野候选、选点**仍然看得见被筛掉的点**（C5）。这是**期望行为**（"应用路线"不该让选点少东西），要在提示语和文档里写清楚，避免以后被当成 bug 修掉。
- 玩家手动改了筛选：以手动为准，**不再回写**路线（避免"我只是想看别的，结果路线被改了"）。

### 3.7 兼容与迁移

| 对象 | 处理 |
|---|---|
| 老自动路线文件（无 `kind`） | 读作 `catalog`；**逐字不变的行为** |
| 老 `SavedRoutes\*.json`（场景→线段） | **一次性导入为手绘路线，原文件移入 `Legacy\`**（§3.2.1）；之后旧读码删除，不再有"只读旁路" |
| 老自动路线文件里的 `formatVersion:1` | 不变 |
| `SavedRoutes\Auto\active.json` | 不变 |
| 新 `Hand\` 目录 | 首次写入时创建；`list` 对缺失目录返回空数组 |
| `FunctionPage`（桌面端） | `AutoRouteSavedRoutes` 的 `ItemsSource` 换成合并列表后仍是 `SavedAutomaticRoute[]`（多三个字段）；**路线名输入框 + 打开路线目录 + 载入按钮删除**（新列表页取代；目录入口在设置页仍有）；`delete` 的确认文案要提到"如果是手绘路线，自由点会一起消失" |
| `CoreHostService` 的 `setRouteName`/`loadRoutes`/`loadRoute` | **删除**（含 `CoreHostMain.cpp:737-751` 的分支与 `FakeServices` 的替身） |
| 发布门禁 | `SavedRoutes` 已在 `ProgramPackageValidation` 保留清单内，无需改 |

---

## 四、改动清单（文件级）

### 4.1 核心（C++）

| 文件 | 改动 |
|---|---|
| `IMao-Core/src/Runtime/RoutePlanningModel.h` | 新增 `StopKind`、`Plan::handDrawn`、`Plan::filterByRoute`、`Key()` 对 free 点的取值 |
| `IMao-Core/src/Runtime/RoutePlanStore.h` | `Save` 写 `kind`；`Load` 对 free 点跳过 resolve；`List` 增加 `stopCount`/`kinds`/`handDrawn`（含 `Hand` 目录合并）；`Validate` 支持 free 点 |
| `IMao-Core/src/Runtime/RoutePlanningService.{h,cpp}` | 新动作 `switch`/`current`/`handStart`/`handPoint`/`handUndo`/`handCancel`/`handCommit`；`save` 支持 `target:"hand"`；`routeId` 校验豁免名单；`SnapshotLocked` 输出 `currentRoute` 与扩展后的 `savedRoutes`；手绘草稿与撤销历史；拒绝路径补日志（C7） |
| `IMao-Core/src/Runtime/HandDrawnRoute.h`（新） | 手绘状态机（纯逻辑、可单测）：追加/撤销/取消/提交、单场景约束、free 编号分配 |
| `IMao-Core/src/Runtime/LegacyHandRouteImport.h`（新） | 旧格式（场景→线段）→ `Plan` 的导入器：保真规则、catalog 端点升级、按场景拆分、幂等、坏文件跳过（§3.2.1） |
| `IMao-Core/src/ImguiDraw/Routes/LoadEditRouteData.{h,cpp}` | **删除**（连同 `DLL_API.cpp`/`Main.cpp`/`CoreHostMain.cpp` 的调用点与三条入口命令） |
| `IMao-Core/src/ImguiDraw/Routes/DrawRouteOnMap.{h,cpp}` | 删 `GetRoutePointsScreen` 与 `#include "LoadEditRouteData.h"`；删 `!automatic` 颜色/粗细分支；加终点"终"字标记 |
| `IMao-Core/src/ImguiDraw/Routes/DrawRouteOnMinMap.{h,cpp}` | 同上（删 `GetRoutePointsScreen`、解耦 include、删分支） |
| `IMao-Core/src/ImguiDraw/Routes/HandDrawnInput.h`（新） | 从旧文件搬来的 Q 轮询线程 + 手绘模式的鼠标钩子分支准备（与 `DrawMarkerInteraction` 协作） |
| `IMao-Core/src/Domain/MapData.h` | 删 `RouteDatas::automatic`（统一后无意义）；`RouteDatas` 降级为**纯渲染 DTO** |
| `IMao-Core/src/Runtime/RoutePlanningModel.h` | `DrawVisibility::Allows` 删掉 `if(!route.automatic) return true;` |
| `IMao-Core/src/ImguiDraw/Items/DrawMarkerInteraction.cpp` | 手绘画布 catch-all 区域登记 + 鼠标钩子分支；free 点的"小圈+数字"与起终点标记；手绘工具栏按钮（**只加在 WinUI 侧，这里仅保留死代码同步**，见 2.1 的教训） |
| `IMao-Core/src/Runtime/MapToolsBridge.h` | 页名白名单加 `routes` |
| `IMao-Core/src/CoreHost/CoreHostMain.cpp` | 删 `setRouteName`/`loadRoutes`/`loadRoute` 三条分支（`:737-751`） |
| `IMao-Core/src/DLL_API.cpp`、`src/Main.cpp` | 删 `LoadEditRouteData` 的 include/Initi/PrepareStorage/三条 API |
| `IMao-Core/tests/RoutePlanningTests.cpp` | 改写 `DrawingVisibilityTests`（不再有 legacy 放行）、`RouteDatas` 构造点；新增导入器用例 |
| `IMao-Core/tests/RoutePlanningServiceTests.cpp` | 见 §6 |
| `IMao-Core/tests/HandDrawnRouteTests.cpp`（新） | 手绘状态机 + 旧格式导入器单测（纯函数，不需要 harness） |

### 4.2 托管（C#）

| 文件 | 改动 |
|---|---|
| `IMao-WinUI/Models/RoutePlanningState.cs` | `SavedAutomaticRoute` 增 `StopCount/Kinds/HandDrawn`；新增 `RouteKindSummary`（nameId/name/icon）；`RoutePlanningState` 增 `CurrentRoute` |
| `IMao-WinUI/Views/MapToolsWindow.cs` | 新页 `routes`；`ShowPage`/`LayoutForGame` 分支；`ShowRoutes` 渲染（行 = 按钮 + 徽标）；`RenderRoute` 增【路线列表】【保存当前路线】 |
| `IMao-WinUI/Services/MapToolsController.cs` | 新命令 `routes`/`saveCurrent`/`switch:<id>`/`delete:<id>` 的分发；应用路线时调用筛选（3.6）；`previousFilterSet` 记录 |
| `IMao-WinUI/Services/IMapToolsController.cs` | 若接口需要暴露新状态则同步 |
| `IMao-WinUI/Services/CoreHostService.cs` | **删** `SetRouteNameAsync`/`LoadRoutesAsync`/`LoadRouteAsync`（`:403-408`）；新命令复用 `ExecuteRoutePlanningAsync` |
| `IMao-WinUI/Views/FunctionPage.xaml` / `.xaml.cs` | 删路线名输入框、打开路线目录、载入按钮（`:95-125` 与 `:64-72`）；列表合并后的展示 |
| `IMao-WinUI/Views/Controls/FilterControl.xaml.cs` | 只读用途：暴露"按 id 批量应用"的公共路径（若 `SetEnabled` 已够用则不动） |
| `Tests/ManagedRuntime/Program.cs` | 改写 `setRouteName`/`loadRoute` 的路径穿越/坏文件用例（`:188-200`）到新入口 |
| `Tests/MainWindowRuntime/FakeServices.cs` | 删三个替身方法（`:85-87`） |

> ⚠️ **教训（写进实现纪律）**：加按钮之前先 grep **绘制函数的调用点**，而不是函数名 —— "看起来像工具栏"和"真的被画出来"是两件事（`DrawMarkerInteraction.cpp:601-606` 的注释就是 2026-09-27 那次返工留下的）。删代码同理：**先删头文件让编译失败**，是最可靠的漏网检查。

---

## 五、实施阶段（每阶段可独立验证、可独立提交）

### 阶段 0：分支与骨架（0.5 天）
- 建分支 `feature/route-list-handdrawn`（**已建**，`main` @ `c9a4ef5`）。
- 写本文件 + 两份勘察报告；§9 三问与 §3.0 的"整体重做"授权均已拍板。
- 验收：无代码改动，只有文档。

### 阶段 1：模型与存储支持自由点（1–1.5 天）
- `StopKind` / `Plan::handDrawn` / `Plan::filterByRoute`；`RoutePlanStore` 的 Save/Load/List/Validate。
- `Hand\` 目录；`List` 合并。
- 验收：
  - 构建并跑**两个**核心目标（`IMaoRoutePlanningServiceTests` 是 `EXCLUDE_FROM_ALL`，必须显式构建，见 §2.7 C6）：
    `IMaoRoutePlanningTests`（默认套件）+ `IMaoRoutePlanningServiceTests.exe <data-dir>`；
  - 新增用例：free 点往返（Save → Load → 逐字段相等）、free 点在 `Load` 时**不查目录**（删掉目录里那个点，仍能加载）、catalog 点校验**没有**被放松（故意改坐标 -> 仍报"点位资源已变化"）；
  - `scripts/Test-Runtime.ps1` exit 0。

### 阶段 1.5：旧数据导入 + 退役旧代码（1–1.5 天）
> 这一步**必须与阶段 1 同批交付**：导入做完之前不能删旧读码，否则老玩家的数据就没人认识了。
- `LegacyHandRouteImport.h`；启动时一次性导入 + 原文件移入 `Legacy\`（§3.2.1）。
- 删 `LoadEditRouteData`、两个 `GetRoutePointsScreen`、三条旧 IPC 入口、`FunctionPage` 的三个控件、`RouteDatas::automatic` 与两处 `!automatic` 分支、`DrawVisibility` 的放行行。
- 验收：
  - **导入按真实数据验**：拿本机 `%LOCALAPPDATA%\IMao-WinUI\SavedRoutes\*.json` 跑，断言条数/点数/`Legacy\` 逐字节等于原文件；重跑一次是**空操作**（幂等）；
  - 坏文件（非 JSON、坐标非有限、线段过长）被跳过且写日志，其余照常导入；
  - **编译证明漏网**：删掉 `LoadEditRouteData.h` 后全量重建 0 error；
  - 导入后的旧路线在**大地图与小地图上都画得出来**（这是"退役旧渲染旁路但不丢数据"的证据）；
  - 老自动路线、`active.json` 恢复、导航、刷怪采集**逐条不变**。

### 阶段 2：手绘状态机与输入（1.5–2 天，风险最高）
- `HandDrawnRoute.h` + 搬过来的 Q 轮询 + 鼠标钩子分支 + catch-all 区域。
- `handStart/handPoint/handUndo/handCancel/handCommit` 命令。
- 验收：
  - 单测：Q 序列 "点1 点2 点3" -> 3 个标记、序号 1..3、起点/终点标记正确；撤销/取消/跨场景拒绝；
  - 真机：大世界里按 Q + 点击若干次 -> 地图上出现蓝色实线 + 自由点数字圈（**这是本阶段唯一能证明技术风险消失的证据**）；
  - 用 `%LOCALAPPDATA%\IMao-WinUI\Logs\` 的新增诊断行核对每次 `handPoint` 的 scene/x/y/命中 id。

### 阶段 3：路线列表页与切换（1–1.5 天）
- `routes` 页（含 `MapToolsBridge` 白名单、窗口尺寸）、列表徽标、当前路线行、保存/删除/切换。
- 验收：
  - `Tests/MapToolsRuntime` 的集成用例：打开列表 -> 选中 -> 切换成功（活动路线 id 变化）；
  - `Test-WinUINavigation.ps1` 仍通过；
  - 真机：切换后大地图上的路线与筛选同时按预期变化。

### 阶段 4：按路线筛选（0.5–1 天）
- 应用路线时收敛筛选 + 开关 + 恢复。
- 验收：
  - 托管单测：`SetEnabled` 被调用成"关掉其它、打开清单"；清单击中图标存在；
  - 真机：应用"叮叮咚 + 宝箱"的路线后，地图上只剩这两类点位与之对应的图标。

### 阶段 5：收尾（0.5–1 天）
- 桌面端 `FunctionPage` 列表合并 + 文案；删掉三个旧控件。
- 文档：把 §3/§5 的"计划"改成"已实现"，按 `MEMORY.md` 的纪律补 §2 状态、§6 新 K 条目。
- 全量 `scripts/Test-Runtime.ps1`；`x64\Release` 与 `out\map-test` 刷新（`Refresh-MapTestBinaries.ps1`）。
- **不做**：不发版、不动 `Version.props`（等用户发话）。

**总估**：6–8.5 个工作日（比初稿多 1–1.5 天，就是阶段 1.5 的导入 + 退役）。

---

## 六、测试计划（具体到用例名）

### 6.1 核心（`IMaoRoutePlanningTests` / `IMaoRoutePlanningServiceTests`）

1. `an older route file without a kind still loads as catalog stops`（回归，钉住向后兼容）
2. `a free point round-trips through save and load unchanged`
3. `loading a free point never consults the point catalog`
4. `a catalog stop whose coordinates moved still fails to load`（**防"为了让自由点通过而放松了 catalog 校验"**）
5. `hand-drawn numbering is stable and one-based across undo`
6. `a hand-drawn route rejects a point from another scene`
7. `list merges automatic and hand-drawn routes and reports stop counts and kinds`
8. `switch makes the route active and runs the navigation; load only loads`
9. `switching a route is not blocked by the active-route id fence`
10. `saving the current draft as a hand-drawn route writes under Hand/<profile>`
11. `a route with only free points reports no filterable kinds`
12. `a rejected route command writes one structured log line`（C7；`RoutePlanningService.cpp:710/:711`）

### 6.1b 旧数据导入（`HandDrawnRouteTests.cpp`，纯函数）

19. `every legacy segment becomes two free points in order`
20. `a legacy endpoint that matches a catalog point is imported as that catalog point`
21. `a legacy file spanning two scenes is split into one route per scene`
22. `importing moves the original file into Legacy/ byte-for-byte and is idempotent`
23. `a corrupt legacy file is skipped with a log line and does not block the rest`

### 6.2 托管（`Tests/ManagedRuntime/RoutePlanningTests.cs`）

13. `saved route rows carry stop counts and point kinds`
14. `applying a route narrows the filter to its kinds and keeps their icons`
15. `applying a route with no kinds leaves the filter untouched`
16. `the previous filter set is restored when the route filter switch is turned off`
24. `the retired route-name and load-route commands are gone from the IPC surface`（防"删了 UI 忘了删通道"）

### 6.3 工具窗集成（`Tests/MapToolsRuntime`）

17. `the route list page opens, lists rows, and switching a row changes the active route`
18. `the route list page passes the native page whitelist`（钉住 `MapToolsBridge.h:104`）

### 6.4 真机验收（用户本人）

- **导入**：升级后打开路线列表 -> 以前手绘的线**还在**，且成了可切换的路线（名字来自旧文件名）。
- 手绘：Q + 点击 × 5 -> 5 个标记，第 1 个"起"、第 5 个"终"，中间空白处是小圈数字，类型点上是图标点。
- 列表：打开 -> 看到多条路线与各自的类型徽标/点数 -> 切换 -> 顶部"当前路线"跟着变。
- 筛选：应用后地图只剩该路线的类型；关掉开关后恢复。
- 回归：自动规划（选点 -> 生成预览 -> 开始指引 -> 刷怪采集开关）**逐条不变**。

---

## 七、风险与对策

| # | 风险 | 对策 |
|---|---|---|
| R1 | **鼠标点击到不了核心**（游戏聚焦、我们的窗口不接收点击） | 复用底层鼠标钩子 + 当帧命中区域（`:418-534`、`:584-589`）。钩子只认 100ms 内登记的区域，所以"截图卡住时点击"天然被拒。**阶段 2 先用真机证明**，再往下做 |
| R2 | **导入吃掉/弄丢老玩家的手绘数据**（本次新引入的最大风险） | 原文件**移动**而非删除（`Legacy\` 逐字节保留）+ 幂等 + 坏文件跳过不阻塞 + 用例 19-23 + 真机验收第一项就是"升级后旧线还在" |
| R3 | 放开 `Load` 校验会顺手放松 catalog 校验 | 用例 4 专门钉住"catalog 点坐标漂移仍报错" |
| R4 | `routeId` 前置校验挡住 `switch` | 显式豁免名单 + 用例 9 |
| R5 | 新页被原生页名白名单拒绝 | 用例 18 钉住；`MapToolsBridge.h:104` 与 `MapToolsWindow.ShowPage` 必须同改 |
| R6 | 手绘期间有活动路线导致状态打架 | 手绘与规划互斥；按 §9 Q2：有导航时只提示、不打断、不接管 |
| R7 | 按路线筛选是**破坏性**地改玩家筛选 | 记录 `previousFilterSet` 并支持一键恢复；清空清单时不动筛选 |
| R8 | 窗口高度在低分辨率下不够放下列表 | `LayoutForGame` 已有 `Math.Min(wantedHeight, rect.Bottom/scale - 96)` 与 `routeScroll` 的 `ScrollViewer`；列表页复用 `ScrollViewer` |
| R9 | **"点到已有点位就连它"的判定不像预期**（`Hit` 只覆盖当帧登记区域，且不存在地图空间 POI 查询，C2） | 按 §2.7 C2 的两步口径（先 `Hit`、再在当帧 `frame.markers` 里按 `radius+4` 取最近）；真机专门验"点在图标边缘、点在两个图标之间、点在图标中心"三种 |
| R10 | **删 `RouteDatas::automatic` 时漏掉读取点** | 先删头文件让编译失败；绘制两个分支 + `App.cpp:2698,2718` + 用例 341-342 是全部读取点（已 grep 确认） |
| R11 | 导入器把"当年画在点位旁边"的线强行升级成 catalog 点（坐标其实差一点） | 升级只认 **≤1e-6**（与 `RoutePlanStore` 同一口径）；差一点就留 free 点，**宁可少认也不错认** |


---

## 八、明确不做（本次范围外）

- 不做跨场景的手绘路线（一条路线一张地图；旧文件里的跨场景数据按场景**拆成多条**）。
- **不做**"旧线段格式与新格式长期并存"的兼容层——旧数据一次性导入后，旧读码与旧渲染旁路**删除**（§3.0）。
- 不改自动路线的 catalog 严格校验。
- 不动 `farmMode` / 实时规划 / 攻略回退等既有行为。
- 不改 `Version.props`、不发版。
- 桌面端 `FunctionPage` 只做"删掉旧控件 + 列表能显示合并结果"，**不重做**它的 UI。
- 不给手绘路线做"画到一半能编辑中间某个点"（本次只有追加 / 撤销一个 / 取消；编辑留到以后）。

---

## 九、已拍板的三件事（2026-09-28）

| # | 问题 | 结果 |
|---|---|---|
| Q1 | 手绘怎么"结束" | **工具栏按钮【完成手绘】**（与自动路线工具栏一致；Q 保持纯"记一个点"语义） |
| Q2 | 手绘时若已有路线在导航 | **只提示、手绘结果只保存不接管导航**（既有导航状态完全不受影响） |
| Q3 | "按路线筛选"开关的默认与作用域 | **默认开、记在路线自己身上**（`Plan::filterByRoute`），并在内存里保留应用前的筛选集合以便恢复 |

> 按 Q2：手绘期间**不**暂停、**不**切换、**不**打断正在导航的路线；`handCommit` 只落盘 + 刷新列表，
> 想走它得玩家自己在列表页点一下（那时才 `switch`）。真机验收要专门验这一条。

### 第四件：手绘整体重做（用户授权，2026-09-28）

> 用户原话：*"手绘路线其实是老项目的遗留。如果你觉得重做这个功能对于以后这个项目的长久发展更好，
> 那你也可以重做这个功能。"*

**我的答复是：应该重做，并且不止是"重写这个功能"，而是把旧的那套数据通路从架构里拿掉。**
判断依据不是"旧代码丑"，而是它**与全项目的承重不变量方向相反**（§3.0 的六维对照表）：

1. **它的数据单位是裸坐标，不是点位身份。** 于是它天生拿不到类型、图标、完成状态、跳过、刷怪采集、
   按路线筛选——也就是本次要做的全部功能。修补它 = 在错误的抽象上叠加六个特例。
2. **它的存储是"全项目共用两个文件"，不是一个路线一个文件。** 于是"路线列表 / 切换 / 删除 / 改名"
   在手绘上**根本无法用现在的存储表达**，必须重造一套（那就是第二个 `RoutePlanStore`）。
3. **它的绘制是一条绕过全部可见性裁决的旁路**（`Allows` 第一行直接放行 `!automatic`），
   并且让绘制层多背了一条红色分支——用户要的"样式一样"，正确修法是**删掉分支**，不是让手绘伪装成自动。
4. **它零测试、零日志**，而它要改的恰好是"鼠标点击 + 按键轮询 + 多线程 + 磁盘写入"。

重做之后，"手绘"不再是第二种路线，而是**同一种 `Plan` 的一种来源**：一个 `handDrawn` 标签 + 允许 free 点。
净效果是代码更少（删一整个类、两个投影函数、三条 IPC、三个桌面控件、两处绘制分支、一个结构体字段），
而功能更多（列表/切换/删除/改名/完成状态/跳过/刷怪采集/筛选全部免费获得）。

---

## 十、实施记录（每阶段续写）

### 阶段 1 + 1.5：模型、存储、旧数据导入（已完成，未提交）

**已实现**

| 位置 | 内容 |
|---|---|
| `IMao-Core/src/Domain/MapData.h` | `enum class StopKind { Catalog, Free }`；`MapLayerIdentity::stopKind`（默认 `Catalog`） |
| `IMao-Core/src/Runtime/RoutePlanningModel.h` | `IsFreeStop()`、`Kinds()`（去重后的点位类型，free 点不参与）；`Plan::handDrawn`、`Plan::filterByRoute`；**`DrawVisibility::Allows` 删掉了 `if(!route.automatic) return true;` 旁路** |
| `IMao-Core/src/Runtime/RoutePlanStore.h` | 构造参数改为 **SavedRoutes 根**（兼容旧的 `…/Auto` 写法）；`Auto/`+`Hand/` 两个目录、`active.json` 带 `handDrawn` 以便恢复；每个 stop 落盘 `kind`；`Load` **只对 free 点跳过目录解析**、`Load` 会在两个目录里找；`Delete` 返回它删的是哪一类；`List` 合并两侧并新增 `stopCount`/`kinds`/`handDrawn`/`corrupt`；`Save/Load` 支持 `handDrawn`/`filterByRoute` |
| `IMao-Core/src/Runtime/LegacyHandRouteImport.h`（新） | 纯逻辑导入器：旧 `{场景: [线段…]}` → `Plan`；每个端点一个 free 点；端点若**恰好**落在已知点（容差 `1e-6`）则升级为该点；跨场景拆成多条 |
| `IMao-Core/src/Runtime/LegacyHandRouteImportFile.h`（新） | 文件侧：扫描 `SavedRoutes` 顶层 `*.json` → 导入进 `Hand/<profile>/` → **原文件移动到 `Legacy/`**；幂等（顶层没有文件就是无事可做）；坏文件跳过并写日志，不阻塞其余 |
| `IMao-Core/src/Runtime/RoutePlanningService.cpp` | 启动时（`Initialize`）在目录建好后、`SyncProfileLocked` 之前跑一次导入，并写 `legacy-route-imported/-skipped/-failed` 日志 |

**实测（本对话亲自跑的）**

- `IMaoRoutePlanningTests`：`Route planning tests passed`（新增 `HandDrawnStoreTests` / `LegacyImportTests` / `LegacyImportFileTests` / `RealLegacyFixtureTest` 共 30+ 条断言）
- `IMaoRoutePlanningServiceTests`：`RoutePlanningService harness failures=0`（含启动路径）
- **真实旧数据**：本机 `%LOCALAPPDATA%\IMao-WinUI\SavedRoutes\Routes.json`（2882 B、`World` 场景、13 条线段、26 个互不重复的端点）
  已复制为固定夹具 `IMao-Core/tests/fixtures/LegacyHandRoutes.json`，用例断言"13 段 → 26 个按绘制顺序编号的 stop、坐标逐位不变"。

**与计划的两处偏差（都是实施中发现后改的，比原计划更安全）**

1. **`Load` 要能同时在两个目录里找到路线**：计划里 `Load` 只看 `Auto/`，实测发现手绘路线永远加载不出来（`无法读取自动路线文件`）。已改为先查 `Hand/` 再查 `Auto/`；`active.json` 额外记 `handDrawn`，恢复时不必探测两个目录。
2. **"缺 `kind` 字段"的读法**：计划只写了"缺省 = catalog"。实测发现这对**含 free 点的文件**行不通——按 catalog 读会拿 `8:free:1` 去查目录，必然失败。所以定成：
   **纯 catalog 点的老文件照常加载**（向后兼容成立），**含 free 点但没有 `kind` 的文件整条拒绝**（既不猜"free"从而悄悄丢掉类型，也不猜"catalog"从而悄悄丢掉点位）。两条都有用例钉住。
3. **旧文件封存位置**：归档在 `SavedRoutes/Legacy/`（与 `Auto/`、`Hand/` 平级），而不是某个 profile 目录里——旧文件本身不属于任何档案，放在公共归档处才不会在换档案后"消失"。

**顺手记下的两个坑（下次别再踩）**

- `store.Save(plan, true)` 一度把 `active.json` 写到 `SavedRoutes/<profile>/` 而不是 `SavedRoutes/Auto/<profile>/`：根目录改成 SavedRoutes 之后，凡是"指向 active 指针"的路径都必须显式走 `Root(profile,false)`。**现象是 StoreTests 里 `CreateFileW` 打不开 active 指针**，不是明显的功能失败。
- 测试里写 resolver 时容易忘记 `sceneId` 参数：点位 key 的前缀是 `layer.stateId`（如今州 8），而 `plan.sceneId` 是运行时场景号（大世界都是 1），两者**不是一回事**。resolver 若用 `sceneId != plan.sceneId` 判断就会永远返回空，报错却是"点位资源已变化"，方向完全反了。**已把测试里的 resolver 全部改成与被测 plan 解耦的显式实现。**

### 阶段 1.5（续）：旧通路的退役（已完成）

`LoadEditRouteData.{h,cpp}` 整个删除；`DrawRouteOnMap/DrawRouteOnMinMap` 的 `GetRoutePointsScreen`、
`Snapshot()`、静态 `routesDatas`/`routeMutex`、`ClearRountsData()` 全部删除（它们的唯一写入者就是被删的投影函数）；
`RouteDatas::automatic` 删除，绘制里的红色分支一并消失——**现在的颜色/粗细/虚线就是原来 `automatic == true` 那一支**
（地图：`previousTarget` 灰虚线 → `preview` 蓝虚线 → `emphasized` 琥珀 3.5px → 蓝 2.0px；小地图同色、3.0px/2.0px、虚线 12/7）。
`DLL_API` 的三个导出（`SetSavedJsonRouteName`/`LoadJsonRoute`/`LoadOneJsonRoute`）与 `CoreHostMain` 的三条管道命令一并删除；
托管侧 `CoreHostService` 的三个方法、`FunctionPage` 的三个控件、`FakeServices` 的三个替身同步删除（资源键保留，避免动本地化脚本）。
`App.cpp` 不再从静态表播种 `frame.mapRoutes`：帧自己持有它投影出来的线段。

> ⚠️ **退役带出一个数据迁移缺口，已补回**：旧类里有一处副作用——把**便携安装**（程序目录旁的 `SavedRoutes\*.json`）
> 复制进 `%LOCALAPPDATA%\IMao-WinUI\SavedRoutes`。删掉它之后，便携安装的旧手绘路线就再也不会被导入。
> 现在这段复制**明确写在导入之前**（`RoutePlanningService::Initialize`，用 `GetModuleFileNameA` 取程序目录）。

### 阶段 2：手绘路线（已完成）

- `IMao-Core/src/Runtime/HandDrawnRoute.h`（新）：`HandDrawnDraft` 状态机。`Start(scene, stateId)` 显式收**两个 id**
  （运行时场景号 + Kuro stateId）——这是本阶段最容易混淆的地方；`Add` 对 free 点分配 `free:N` 编号并保证撤销后编号无空洞；
  `Commit` 生成 `handDrawn=true` 的 `Plan` 并把首个点当起点。
- **取点**：`App::PollHandDrawnRoute()`，每帧一次按下沿，读 `TryGetRoutePoint`（与旧工具同一处读数）→ `handPoint`。
  它是**每按一次记一个点**，不是旧工具的"按两下一次成段"。
- **点击**：底层鼠标钩子在 `handDrawMode` 为真时把大地图上的左键吞掉并转成 `route:hand:point`，
  再由 `DrawMarkerInteraction` 换算成 ROC 后发给 `handPoint`——**这一条不依赖旧的命中区域表**
  （手绘模式不登记任何区域），改用"地图画面是否新鲜"作为判据，与规划画布本身同源。
- **落点判定**：`handPoint` 只收坐标；"是否点在已有点位上"由核心在 ROC 空间按 `HandDrawSnapRoc = 3.0`
  （≈3.6 地图像素，约为图标半径的三分之一）判定——**故意收紧**，宁可是自由点，也不要因为手一抖就给路线安上一个它并不关于的点位类型。
- **渲染**：草稿通过 `RoutePlanningView::handDraftPreview` 以 `Plan` 形态交给 `App.cpp` 的 `appendRoute`，
  因此**与自动路线完全同一套绘制代码**（同色、同宽、同编号徽标、同起点标记），没有第二条渲染路径。
- **悬停提示**：`DrawMarkerInteraction::DrawMap` 在手绘时把光标位置画成"圈 + 十字 + 按 Q 记下这里/这个点位"。
  之所以必须有它：取点是读光标而不是读点击，没有提示玩家无法判断自己会连到点位还是丢一个编号标记。
- **取消点**：离开大地图（`MapUnavailable`）、切档案（`SyncProfileLocked`）、定位停止（`SessionStopped`）都会取消手绘。

### 阶段 3 + 4：路线列表页与按类型筛选（已完成）

- 核心：新增 `switch`（= load + 立刻开始指引）与 `current`；`save` 支持 `handCommit` 之后的手绘路线；
  `SnapshotLocked` 输出 `currentRoute`（活动路线，否则草稿预览）与 `handDrawnActive/handDrawnCount`；
  `PlanJsonLocked` 带上 `handDrawn`/`filterByRoute`；`RoutePlanStore::List` 每行带 `stopCount`/`kinds`/`handDrawn`/`corrupt`。
- 托管：`MapToolsWindow` 新增 `routes` 页（当前路线行 + 保存/删除 + 按类型筛选 + 刷新 + 每行名称/点数/地图/来源/类型徽标 + 手绘控制），
  `LayoutForGame` 给该页 640 高，`MapToolsBridge` 的页名白名单加 `routes`（**不同步改这一行就会被原生拒绝**）。
- 类型徽标用的是 `FilterSelectionService.Catalog` 的同一个 id → **同一条身份链**：路线列表里的图标和
  「点位筛选」里的图标是同一张图，不需要新的映射表。
- **按类型筛选**：`MapToolsController.ApplyRouteFilterAsync` —— 只发差集（核心的筛选注册表只追加不去重）、
  应用前把玩家原有筛选存进 `filterBeforeRoute`（仅内存），再点一次即恢复。手绘路线若只有自由点则**不动筛选**并给出提示。

### 真机数据上的导入实测（2026-09-28，**生产代码路径，不是模拟**）

用户本机那份旧的 `%LOCALAPPDATA%\IMao-WinUI\SavedRoutes\Routes.json` 在本次开发期间**被真实导入过**，结果：

```
Hand\local\legacy-Routes.json   id=legacy-Routes name=Routes sceneId=1 handDrawn=true filterByRoute=true
                                stops=26  start.source=legacyImport
                                kinds: free=26（全部是自由点，没有端点命中官方点位）
Legacy\Routes.json              与 IMao-Core/tests/fixtures/LegacyHandRoutes.json SHA-256 逐字节相同
```

即：**26 个按绘制顺序编号的点、坐标原样、原文件一个字节没动地进了归档**。
（原文件在用户本机曾一度消失，是因为导入把它**移走**了——这正是设计行为；已用夹具还原。
`fixtures/LegacyHandRoutes.json` 就是那份文件的副本，长期留作回归夹具。）

### 导入落点：为什么导入给"每个用过的档案"各来一遍

⚠️ `DrawItemBase::MarkerProfile()` 在启动时可能是 `local`，而玩家真正在用的是账号档案（本机实测是 `acc_2b60810e`）。
只导入启动时那一个档案，"档案对不上"就会让玩家的旧路线**存在于磁盘上但在界面上看不见**。
所以导入会覆盖：`SavedRoutes\Auto\` 下**每一个已存在档案目录**（这是"确实在用"的证据）+ 当前档案 + `local`。
只有**全部目标都成功**才把原始文件移进 `Legacy\`，任何一个失败就留在顶层，下次启动重试——不会出现"归档了但没导进去"。

### 补充测试与两个被测试抓出来的**真 bug**（2026-09-28 第二轮）

计划 §6 里点名的遗漏覆盖补上了：

- `IMao-Core/tests/HandDrawnRouteTests.cpp` → 新目标 **`IMaoHandDrawnRouteTests`**（已加进 CMake 与 `add_test`）：
  草稿状态机（编号、撤销后编号无空洞、跨地图拒绝、起点=首点、单点不能提交）+ 导入器（逐段两点、只升级恰好命中的端点、跨场景拆分、坏文件拒绝）。
- `Tests/ManagedRuntime/RouteFilterPlanTests.cs` → 把筛选决策抽成可测的纯逻辑 `RouteFilterPlan`
  （`Narrow` 只发差集、`Restore` 只发移动过的项）。抽出来是为了让"**发了什么进去**"能被断言，
  而不只是"结果对不对"——核心的筛选注册表只追加不去重，发多了会一直涨。
- `Tests/ManagedRuntime/RoutePlanningTests.cs` 新增一段真 IPC 用例：写一条 `Hand/local/<id>.json` →
  `list` 能报出点数/地图/来源/自由点无类型 → `switch` 应用并开始指引 → **重启后仍能从 `active.json` 恢复**（证明指针记得住目录）。
- `Tests/ManagedRuntime/Program.cs` 里原来测 `setRouteName`/`loadRoute` 的用例改写成走新入口。

写这些用例的过程中抓到两个**真实缺陷**，都不是测试写法问题：

1. **`switch` 被活动路线 id 围栏挡住**。`RoutePlanningService::Command` 的前置校验要求"带 `routeId` 的命令必须指向当前活动路线"，
   而 `switch` 的整个语义就是**换到另一条**路线——不豁免就永远拒绝，界面上表现为"点列表里的任何一行都没反应"。
   已把 `switch` 加进 `load`/`delete` 那一侧的豁免名单（计划 §3.3 预警过这一条，实测确认它真的会挡）。
2. **`currentRoute` 的线上形状两端不一致**（更严重，UI 会静默显示空）。核心发的是
   `{"route": <plan>, "preview": bool}`，而托管侧 `RoutePlanningState.CurrentRoute` 期待的是 **plan 本身**，
   于是反序列化出来的 `CurrentRoute.Id` 是空字符串——**列表页顶部那一行会显示"当前路线：还没有路线"，而实际上有**。
   已改成核心直接发 plan（或 `null`），预览标记另走 `currentRouteIsPreview` 一个字段。
   ⚠️ 教训：IPC 结构的"嵌套一层"和"少一个字段"都不会报错，只会让 UI 安静地少显示东西；**加形状断言比加功能断言更值**。

### 实机反馈与修复（2026-09-28，第三轮，用户截图）

用户第一次真机跑路线列表，报了 7 条。逐条记下**真因**（不是"调了下样式"）：

| # | 现象 | 真因 | 修法 |
|---|---|---|---|
| 1 | 列表里的图标显示成 `SP_IconMonsterHead_1001_UI` 这样的**原始 id 文本** | 两端**id 空间不同**：路线的点位类型是游戏 items 数据的叶子 id（图标清单覆盖 528 个），而托管侧的 `MapFilterCatalog` 只认识**筛选项**那 211 个。查不到就回落到裸 id | 类型描述改由**核心**给出：`savedRoutes[].kinds[]` 现在带 `{nameId, name, icon}`，名字取 `r.names`、图标取 `DrawItemBase::GetExternalIconPath`（只有核心两边都认识）。托管侧不再自己查目录 |
| 2 | 图标仍然只有文字 | `RouteKindSummary.IconUri` 用 `Uri.TryCreate(path, UriKind.Absolute, ...)`——**它拒绝绝对 Windows 路径**，于是每个图标都被判为"没有图标" | 改用 `new Uri(path)`（双参构造），并对 `UriFormatException` 兜底 |
| 2b | 修完上一条**还是**没有图标 | **第三个独立原因**：`System.Text.Json` 默认按属性名匹配键，`nameId`→`NameId`、`name`→`Name` 都成立，但 **`icon`→`IconPath` 不成立**（多出的 `Path` 后缀不会自动兼容）。图标字段静默丢失 | 显式加 `[JsonPropertyName("icon")]`。⚠️ 这三条**必须都修**才看得见图标：id 空间、URI 构造、字段映射，每一条单独看都"没报错" |
| 3 | 点击手绘不需要 Q 就能加点，但描述还写着 Q | 点击路径（鼠标钩子）本来就生效 | 描述改成"点击加点（也可以按一次 Q）"；**Q 保留为可选方式**，不再宣称它是必须的 |
| 4 | 选到已有点位时没有"已选中"的反馈 | `planningBinding.selectedKeys` 只填 `planning.selected`（选点工具栏的集合），手绘草稿不在里面 | 把手绘草稿里的**已有点位**也填进 `selectedKeys`，于是点位高亮与"已选"标签和选点模式**完全同源**（自由点没有图标，不参与） |
| 5 | 自由点没有编号小圈，也看不到起终点 | 两处都**只认选点模式**：`plan` 只在 `planning.enabled` 时取预览；"起"字标记要求 `planning.enabled`。手绘不进选点模式 ⟹ 整个徽标块被跳过 | `RoutePlanningView` 新增 `handDraft`（与 `handDraftPreview` 同一个 plan，但**不受选点模式约束**）；徽标/起终点改判 `drawing`；自由点的数字**画在点上**而不是偏到右边（没有图标可偏） |
| 6 | 手绘模式退不出去 | Esc 走的是"取消手势 / 回到平移"，手绘不属于这两者 | Esc 在手绘时发 `handCancel`：`planningEscapeRequested` 的消费处先判 `handDrawnActive` |
| 7 | Ctrl+Z 不能撤销 | 没有绑定 | 键盘钩子里新增 Ctrl+Z，**只在手绘进行中**认领该键（其他时候 Ctrl+Z 还是游戏的），走 `handUndo` |
| 8 | 手绘线是虚线、偏细、没有方向 | 草稿以 `preview` 身份渲染，而预览的样式就是虚线 | `RouteDatas` 新增 `handDrawn`：实线、粗一档（地图 4.0 / 小地图 3.5），每段中点画方向箭头。自动路线的样式**逐字未动** |

⚠️ 第 1、2、2b 条连在一起才解释得通：**先**是 catalog 查不到（id 空间不同），**再**是就算查到了也会被 `Uri.TryCreate` 丢掉，
**最后**是就算 URI 对了，`icon` 字段压根没反序列化进来。三条都安静无声——图标只是"没出现"，没有任何报错。
现在每一条都有断言钉住（`RoutePlanningTests.Run` 里的形状测试 + 核心的 `VerifyRouteListShape`）。

**新增依赖**：`DrawItemBase::GetExternalIconPath` 现在被服务层用到，测试替身（`RoutePlanningServiceTestHost.h`）里补了一个返回空串的同名函数——
空串正是真实实现对"清单里没有这个类型"的答案，所以快照形状两边一致。

### 第二轮实机反馈（2026-09-28，第四条）

用户第二次实机跑，三条没修好或新暴露：

| # | 现象 | 真因 | 修法 |
|---|---|---|---|
| 1 | 列表图标**仍然**不显示（名字对了） | 上一轮只修到"路径能到 UI"。真正卡住的是**显示方式**：给 `Image.Source` 一个 `file:///` URI 时，无包标识（unpackaged）的 WinUI 图片加载器**没有文件系统权限**，加载失败且**完全静默**——不抛异常，只是不显示 | 不再交 URI，改为**自己把 PNG 读成流**再 `SetSource(stream.AsRandomAccessStream())`；按路径缓存；失败时走诊断出口而不是静默 |
| 2 | 选到已有点位仍然没有选中状态 | **这一条是我上一轮改错了位置**：`DrawMap` 里有**两处**填 `selectedKeys`，第二处（`planningBinding.selectedKeys.clear()` 之后重建）在我插入的那处**之后**执行，把我加的手绘点位键**清掉了**。徽标能画出来是因为它读的是 `plan`，不读 `selectedKeys` | 把手绘点位的插入移到**那次 clear 之后**，并加注释说明为什么顺序是关键 |
| 3 | Esc 直接退出了游戏大地图 | 手绘时我让 Esc 走 `planningEscapeRequested`，但那条路要求"规划画布可交互"才认领该键；不满足时 Esc **透传给游戏**，于是游戏关掉了自己的大地图 | 键盘钩子里**最前面**显式判 `handDrawnActive`：吃掉 Esc（`return 1`）并置 `handEscapeRequested`，渲染帧再发 `handCancel`。与规划画布是否可交互无关 |

**教训（写进纪律）**：第 2 条的形状是"**同一个状态在一帧里被填了两次，我改的是先那次**"——
`grep` 到目标字段就下手，没看它在同一函数里被重建过。以后给"每帧重建的集合"加内容，先确认**它最后一次被写是在哪一行**。

**留下的诊断线索**（两类静默失败都改了出口）：`map-tools-route-kind`
（`no-icon <nameId>` / `icon-load-failed <path>`）、`hand-drawn-selection`
（`recorded=/markers=/matched=`），都写到 `gamepad` 日志。

**已知未修**：核心把图标路径放进 JSON 时用 `std::filesystem::path::string()`（ANSI 代码页）。
含非 ASCII 字符的路径（例如中文用户名）在 C++20 下应该会抛异常而不是乱码，所以这条**尚未被证实有问题**；
但若用户的图标路径含非 ASCII 且仍然不显示，`map-tools-route-kind` 的 `icon-load-failed` 会给出确切路径，届时改用 `u8string()`。

### 第二轮实机反馈的后续：Esc 必须**保留**手绘结果（2026-09-28）

用户的原始要求是"Esc 只是退出手绘状态"，因为**手绘期间打不开工具栏**（手绘占用大地图的点击）。
我第一版把 Esc 接到了 `handCancel` —— 那个动作的语义是"放弃并清空"，
于是玩家退出手绘后**画好的点直接没了，根本来不及去保存**。

**修法：把"结束"与"放弃"拆成两件事**（这是本轮真正的设计修正，不是调参）：

| 动作 | 语义 | 点位 |
|---|---|---|
| `handFinish`（Esc 走这条） | 结束绘制、**保留**草稿等待保存 | 保留 |
| `handDiscard` | 明确放弃这次手绘 | 清空 |
| `handCancel` | 同上（保留给"放弃"语义的调用方） | 清空 |
| `handCommit` | 保存为路线文件 | 保存后清空 |

配套：`HandDrawnDraft` 增加 **`Pending()`**（"结束了但还没保存"是一个真实状态，不是 `Active()` 的反面）；
`handUndo`/`handCommit` 改为按**点位是否为空**判断而不是按"是否正在绘制"；
`RoutePlanningView` 增加 `handDrawnPending`；快照输出 `handDrawnPending`。

路线列表页据此分三态显示：
- **正在手绘**：`撤销上一个点` / `结束手绘`
- **已结束未保存**：`保存手绘路线` / `放弃这次手绘`（并提示"画好的 N 个点还留着，尚未保存"）
- **空闲**：`开始手绘`

> 之所以**不**给"已结束未保存"提供"继续手绘"按钮：继续就必须重新开始绘制会话，
> 而 `handStart` 会清空草稿——那样这个按钮就成了"悄悄丢掉上一条草稿"。要做真正的续画得另设计，本轮不做。

`handCancel` 与 `handDiscard` 的区别只在报错文案（前者对"正在绘制"更自然），两者都清空。

### 第三轮实机反馈（2026-09-28，第五条）：连接、续画、Esc 的最终定型

用户第三次实机跑，三条：

| # | 现象 | 真因 | 修法 |
|---|---|---|---|
| 1 | **Esc 仍然清空手绘** | ⚠️ **部署问题 + 一处语义遗留**：上一轮我写好了 `handFinish`（保留），但**只跑了单测、没有刷新 `out\map-test`**，用户跑的还是更早那份"Esc → `handCancel`"的二进制。而且当时"继续绘制"确实不存在，所以即便跑对了也没路可走 | 本轮把三态彻底做完并**刷新部署**；`handFinish`/`handDiscard`/`handCancel` 语义分离（见下） |
| 2 | 点击已有点位**能选中但连不上** | 点击路径**只把坐标**发给核心，核心再靠 `HandDrawSnapRoc = 3.0` 的容差反猜是哪个点位。而 `Hit()` 在钩子里**本来就已经知道确切的点位身份**（`p:<pointId>` / `g:<pointId>`）——信息在手上却扔掉了，让下游去猜一个已知的答案 | 把命中身份**直接传下去**：点击命令带 `key = <stateId>:<pointId>`，核心用 `ResolveLocked` 精确解析，不再依赖距离。自由点走原来那条路（没有 key） |
| 2b | 悬停提示与点击结果可能不一致 | 悬停提示用"距离 ≤ 半径"判断、点击用命中区域判断——**两个判据**，会出现"提示说能连、点下去却是自由点" | 悬停提示也改用 `Hit()`，与点击**问同一个问题**。绿圈 = 会连到点位，橙圈 = 会记自由点，文案直接写"点击连接到这个点位 / 点击记下这里" |
| 3 | 缺少"继续绘制" | `handStart` 无条件 `points.clear()`，所以"继续"没法做——做了也等于"重新开始并丢掉草稿" | `HandDrawnDraft::Start` **在同地图上遇到未保存草稿时改为续画**（不清空、编号接着走）；换地图则明确拒绝并提示"请先保存或放弃"。路线列表三态都有按钮：`继续绘制` / `保存手绘路线` / `放弃这次手绘` |
| 3b | 需要"进入绘制状态"的快捷键 | 之前 Q 只在已处于绘制状态时才加点 | `PollHandDrawnRoute` 改成：**不在绘制中 → 发 `handStart`（进入或续画）；已在绘制中 → 记一个点**。Q = 进入绘制模式 + 取点，语义由状态决定，且**永远不会清空手里的草稿** |

**动作语义（最终）**：

| 动作 | 谁触发 | 点位 |
|---|---|---|
| `handStart` | 按钮「开始手绘」/「继续绘制」、Q 键 | 无草稿则新建；有同地图草稿则**续画**；换地图则拒绝 |
| `handFinish` | Esc、按钮「结束手绘」 | **保留**（进入"已结束未保存"） |
| `handCommit` | 按钮「保存手绘路线」 | 写文件后清空 |
| `handDiscard` / `handCancel` | 按钮「放弃这次手绘」 | 清空 |

⚠️ **教训（这轮最重要的一条）**：**"我改好了"和"你跑到的是改好的那份"是两件事。**
上一轮 `handFinish` 写完只跑了 `IMaoHandDrawnRouteTests`，没有 `Refresh-MapTestBinaries.ps1`，
用户因此又白跑一次并复现了旧行为。以后凡是"实机反馈 → 修复 → 等复测"的循环，
**必须把刷新 `out\map-test` 当作修复的一部分**，并在回复里报出部署时间戳。

### 第四轮：重叠点位可选、待保存的线继续显示、工具栏记住上次页面（2026-09-28）

用户第四条反馈（前一条已确认"现在这个不错"），三条：

| # | 需求 | 做法 |
|---|---|---|
| 1 | 现有点位的**重叠点**也要能选中 | 手绘时若**光标附近的候选点多于一个**，直接打开选点工具那套**成员面板**（`expanded`/`expandedMembers`），锚点放在**光标**而不是某个成员上——面板的意义正是"还没能瞄准某一个"。每个成员沿用既有的 `p:<id>` 命中区域，所以点哪一个就精确连哪一个。复用同一套渲染，两种模式行为一致 |
| 2 | Esc 之后**不要清空渲染**，等玩家保存或丢弃再清 | `DrawingVisibility` 原来只在 `Active()` 时把草稿当预览发出去，所以结束即消失。改成**只要有草稿（`Size()>0`，无论进行中还是待保存）就继续渲染**。同时 `Plan` 新增 `handDraft` 标记（**不落盘**），让渲染层能区分"还在画的路径"与"等待确认的规划预览"，实线+箭头样式因此得以保持 |
| 3 | 工具栏折叠后**记住上次的页面** | 之前每次折叠都重建窗口、构造函数里写死 `ShowPage("home")`，所以每次都要从最外层点起。现在 `MapToolsController` 记住 `lastPage`，重建时传回去；**画布工具（移动/框选/套索）故意不记**，因为恢复一个半途的选取状态比多点一次更糟。重新打开在 `routes` 页时顺便补一次 `list`，否则那页没有行数据 |

**关于"重叠"的一个诚实说明**：候选点的判据是**屏幕距离**（`radius + 8`），不是"同一个分组"。
`BuildMarkerLayout` 会合并同一优先级的近邻点，但不同楼层的点优先级不同、不合并，
所以"画在一起但不成组"的点也能被面板列出来——这正是之前点不到的那些。

### 第五轮：Esc 之后覆盖层也要留下（2026-09-28）

用户第五条反馈（截图）：Esc 后**线留住了**，但**起点"起"、终点"终"、自由点的编号小圈、
已有点位的选中高亮全部消失**。用户的说法很准确——"此时此刻还没有保存或者丢弃，还是在预览状态"。

**真因**：上一轮我只把**线段**这一条路放开了（`DrawingVisibility` 不再要求 `Active()`），
但**覆盖层**（`DrawMarkerInteraction` 里画起终点、编号、高亮的那一大块）的开关仍然是
"正在绘制中"。于是"保存或放弃之前都算预览"这条规则只落实了一半。

**修法：把"是否显示"和"是否能加点"拆成两个概念**

| 概念 | 含义 | 用在哪 |
|---|---|---|
| `drafting` = 有草稿（`handDraft.has_value()`） | 从记下第一个点起，直到保存或丢弃 | 起点/终点标记、编号小圈、选中高亮、"已选"标签、自由点判定 |
| `drawing` = `handDrawnActive` | 只有正在接受点击的那一段 | 重叠点位的**成员面板**（那是输入辅助，不是浏览信息） |

也就是说：**结束绘制不该让任何"这是什么东西"的信息消失，只该让"还能不能加点"停下来。**

### 第六轮：选路线后自动切换筛选 —— 让它可见、可撤销、可连续切换（2026-09-28）

用户要求："选择路线后，自动把当前筛选的点位更换为此路线包含的点位类型……对于自由点这种没有图标的，就显示那个带有自由点次序的小圈。"

**这条逻辑本轮之前就接在 `switch` 上了，但用户看不到任何效果。** 两件事导致它"像是没做"：

1. **界面完全没有迹象**：筛选被改了，但列表页、工具栏都没有任何提示，玩家无法判断是"自动切了"还是"没生效"。
   而且按钮文案一直写"按类型筛选点位"，**看不出它其实是"恢复"**。
2. **切换第二条路线时会重复收窄**（真缺陷）：`ApplyRouteFilterAsync` 每次都把"当前状态"记成 `filterBeforeRoute`，
   于是切到 B 路线时，A 路线的筛选被当成了"玩家原本的设置"——**玩家再也回不到自己的地图**。

**修法**

- 拆出两个变量：`filterBeforeRoute`（是否处于"被路线收窄"的状态）与 `RememberedFilters`（**第一条路线之前**的玩家原始筛选）。
  只有第一次收窄才记录，之后无论切几条路线，撤销都能回到最初的玩家设置。
- 收窄始终基于**当前**状态计算差集（`Narrow` 本就是差集），所以 A→B 只改真正不同的那几项，不会来回抖动。
- 列表页新增**可见状态**：一行"当前只显示『<路线名>』用到的 N 种点位（选择路线时自动切换）。自由点没有图标，会一直显示成带编号的小圈。"
- 按钮文案随状态变化：未收窄时是「按类型筛选点位」，已收窄时是「**恢复全部点位**」——一眼看出按下去会发生什么。

**关于自由点**：无需额外处理。自由点本来就不参与筛选（没有类型可筛），它的显示来自**路线覆盖层**的编号小圈，
而那个覆盖层不受筛选影响——筛选只管地图图标。这一点本轮在文档里写清楚，并作为提示语展示给玩家。

### 第七轮：筛选改成"随导航借出、退出导航归还"（2026-09-28）

用户要求："启用已经保存的路线时，先暂存当前筛选状态，再更换为路线的点位筛选状态，退出此路线导航时，恢复之前暂存的筛选状态。**只针对已经保存的路线。**"

这条把筛选的语义从"应用路线时改一次"正式收窄为**与导航同生共死的一次借出**。上一轮的实现有两处不符合：

1. **借出没有归还的触发点**：只有手点按钮才恢复，`退出导航` 恢复不了。
2. **暂存只在内存里**：筛选偏好本身是**落盘**的，所以"导航中途关掉程序"会把收窄后的筛选当成玩家自己的选择存下来——下次启动就是一张被永久筛过的地图。

**修法**

| 概念 | 实现 |
|---|---|
| 借出 | `RouteFilterSnapshot.Begin(routeId, 当前筛选)`：**只在没有借出时记录**，所以连续切路线永远回到"玩家最初的地图" |
| 借出对象 | 记录 `routeId`；`loanedRouteId` 决定"哪条导航结束时归还" |
| 归还 | `EndRouteFilterLoanIfNavigationEnded()`：看核心快照里 `Active` 是否还等于借出时那条路线。**不关心是谁结束的**——`退出导航`、删除路线、核心丢弃坏路线，都走同一个判断 |
| 落盘 | `%LOCALAPPDATA%\IMao-WinUI\RouteFilterLoan.json`（与 `FilteredItemsData.json` 同目录）。**借出必须落盘**，否则借出比被借出的东西还短命 |
| 重启 | 构造 `MapToolsController` 时若发现未归还的借出，重新接管它——这样"导航中途重启"之后，那条导航结束时仍会归还 |
| 只针对已保存路线 | 借出只发生在 `switch`（点列表里的已保存路线）。预览与手绘草稿**不借出**，因为它们还不是"要跟着走的路线" |
| 无类型可筛 | 全自由点的手绘路线 `kinds` 为空，**直接不动筛选**并给出提示（保持上一轮的行为） |

手动的「按类型筛选点位」按钮**已去掉**：筛选现在跟着导航走，没有需要手动撤销的东西；按钮位置改成一条说明（导航中会提示"退出导航后自动恢复"）。列表页的筛选状态行也改成明确的"导航中……退出导航后会自动恢复到你自己原来的筛选"。

**新增测试**（`RouteFilterPlanTests`）：借出前没有借出、第二条路线复用而**不覆盖**记忆、归还后清空（不会重复恢复）、**损坏的借出文件被忽略**（否则会还给玩家一个他从未有过的筛选）。

### 最终验证（本对话亲自跑的，全绿）

| 检查 | 结果 |
|---|---|
| `ctest -C Release`（**全部 7 个**，含新目标） | **100% tests passed, 0 failed out of 7** |
| `IMaoHandDrawnRouteTests`（新目标：草稿状态机 + 导入器） | `Hand-drawn route tests passed` |
| `IMaoRoutePlanningTests` | `Route planning tests passed` |
| `IMaoRoutePlanningServiceTests` | `RoutePlanningService harness failures=0` |
| `IMao-CoreHost` 全量重建 | 链接成功，只有既有的 C4244 警告 |
| `IMao-WinUI`（自包含 + 普通）| 0 error |
| `ManagedRuntime`（真 IPC，起真 CoreHost）| `All managed runtime tests passed` |
| `scripts/Test-Runtime.ps1` 全量闸门 | **exit 0**，`Runtime tests passed` |
| `out/map-test` | 已按本次构建刷新（自包含，`layered-floor` 10/10 ok） |

> ⚠️ 夹具路径的坑：`RealLegacyFixtureTest` 一开始按**当前工作目录**找夹具，从仓库根目录手动跑没问题，
> 但 `ctest` 是在构建目录里运行二进制的，于是只有 ctest 会红。已改成**编译期传入源码根**
> （`IMAO_TEST_SOURCE_DIR`，由 CMake 定义），工作目录只作为回退。
> 教训：**测试必须能在 ctest 下通过，而不只是"我在根目录手跑过"**——这也是 `Test-Runtime.ps1` 与 `ctest`
> 两条腿都要走一遍的原因。

**仍未做的**：真机验收（手绘取点/点击、列表切换、按类型筛选、旧路线在列表里可见）——**需要用户本人跑一次**，
因为"鼠标点击能不能到达核心"只能在大地图上证明。日志线索：`events-*.jsonl` 里的
`hand-drawn-point`（每次取点，带 `kind=free|catalog`）、`hand-drawn-committed`（提交）、
`route-command-refused`（任何被拒绝的路线命令，带 action 与原因）、`legacy-route-imported/-skipped/-failed`。

---
