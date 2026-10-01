# 路线合集与路线包导入导出 · 方案

日期：2026-10-01
分支：`feature/route-collections-import-export`（自 `main` @ `4f32e09`）
状态：**方案 · 未实施**（本轮交付物就是这份计划；实施时按 §6 逐阶段续写 §11 实施记录）

---

## 1. 需求（用户原话，逐字）

> 新开一条分支，用于为路线列表添加新功能。1.请你添加路线合集功能，要求能够创建合集、删除合集，能够将路线加入合集。现有的路线都归为默认合集。在保存自动路线和手绘路线时，默认保存到当前所在合集。也可以对合集中的路线进行移动到其它合集。 2.添加路线导入导出功能，默认为批量操作，点击批量导出按钮后，路线前面出现选择框，可以进行勾选。也可以选择导出合集。对于导入，则可以区分批量导入路线或者导入合集，如果是导入路线则默认导入当前选择的合集，如果是导入合集，则创建合集并导入合集中的路线，同时切换到此新合集，同名合集则弹窗提示选择覆盖还是新建。

凡本文与用户原话冲突，以原话为准。

## 2. 本轮已拍板（用户已回答，不要再问）

| # | 问题 | 决定 |
|---|---|---|
| D1 | 功能放在哪个界面 | **两边分工**：桌面「路线」页 = 管理面（合集增删改、批量勾选、导入导出、文件对话框、弹窗）；游戏内工具窗「路线列表」= 只加**合集切换 + 当前合集显示**，让「保存到当前所在合集」成立 |
| D2 | 删除合集时里面的路线 | **连同路线一起删除**（确认框写明将删除几条、叫什么） |
| D3 | 导入同名合集选「覆盖」的含义 | **整体替换该合集**：先删掉该合集现有路线，再换成导入的这一批 |
| D4 | 导入入口 | **一个「导入」按钮**，由文件内容（`kind`）自动分流；选完文件后弹一句确认 |
| D5 | 批量勾选能做什么 | **导出所选 + 移动到合集 + 删除所选**（通用批量模式） |
| D6 | 「默认合集」能否改名/删除 | **固定名字、不可删除**（永远存在，是老路线与删合集后的兜底） |

## 3. 已核实的事实（全部带证据，本轮只读调查得出）

### 3.1 路线的所有权与落盘

| 事实 | 证据 |
|---|---|
| 核心拥有路线的排序、完成与持久化；托管侧快照只是显示 | `IMao-WinUI/Models/RoutePlanningState.cs:5` |
| `RoutePlanStore` 是唯一的落盘实现，根目录 = `%LOCALAPPDATA%\IMao-WinUI\SavedRoutes` | `IMao-Core/src/Runtime/RoutePlanStore.h:36,131-135`；`RoutePlanningService.cpp:567` |
| 自动路线 `SavedRoutes\Auto\<profile>\<routeId>.json`；手绘 `SavedRoutes\Hand\<profile>\<routeId>.json`；活动指针 `Auto\<profile>\active.json` | `RoutePlanStore.h:136-144` |
| 路线 id = 文件名；校验规则：非空、≤96 字符、只允许 `[A-Za-z0-9_-]`；保留字 `active`（大小写不敏感） | `RoutePlanStore.h:14-21,146-151` |
| 路线文档字段：`formatVersion,id,name,profileId,sceneId,start,stops,skipHistory,farmMode,handDrawn,filterByRoute` | `RoutePlanStore.h:39-59` |
| 读取上限 2 MiB；`Validate` 要求 name ≤256、已知场景、`start.valid`、1..500 个点、key 唯一 | `RoutePlanStore.h:152-156,230-245` |
| 删除 = `MoveFileExW` 到 `.deleting` 墓碑（提交点），随后清理 | `RoutePlanStore.h:204-229` |
| `List(profile)` 把 Auto/Hand 两个目录合并成扁平数组，按 id 排序；坏文件仍出现且标 `corrupt` | `RoutePlanStore.h:93-128` |
| 一次性旧手绘导入只看 `SavedRoutes` **顶层普通文件**，不递归子目录 | `LegacyHandRouteImportFile.h:28-32` |

### 3.2 命令面

| 事实 | 证据 |
|---|---|
| `RoutePlanningService::Command` 是 if/else 链，未知 action 直接抛 | `RoutePlanningService.cpp:853-1009`（拒绝点 `:998`） |
| **前置围栏**：带 `routeId` 的命令必须等于当前活动路线，`load`/`delete`/`switch` 在豁免名单里 | `RoutePlanningService.cpp:865-866`（`switch` 曾因漏加豁免在生产被静默拒绝） |
| 每次命令都 `++r.revision` 并 `Emit()` 推一次快照 | `RoutePlanningService.cpp:999,1008,356-362` |
| `save`：`target=="preview"` 或没有活动路线时存预览，否则存活动路线；`store->Save(plan,!preview)` | `RoutePlanningService.cpp:967-973` |
| **`target:"hand"` 代码里不存在**（文档写了、代码没实现），手绘只走 `handCommit` | `RoutePlanningService.cpp:243-259,967-973` |
| `handCommit` 用 `NewId()` 生成 id、`handDrawn=true`、`Save(plan,false)` | `RoutePlanningService.cpp:96-104,246-247` |
| 切换档案：`SyncProfileLocked()` 重读 `List(profile)` 与 `LoadActive(profile)` | `RoutePlanningService.cpp:363-378` |

### 3.3 界面

| 事实 | 证据 |
|---|---|
| **游戏内大地图工具窗是真窗口**，`MapToolsController.OpenAsync` 是唯一生产构造点 | `MapToolsWindow.cs:19`；`MapToolsController.cs:136` |
| 游戏内路线工具栏 = `RenderRoute`；路线列表页 = `RenderRoutes` + `BuildSavedRow` | `MapToolsWindow.cs:199-255,261-353,361-384` |
| 页面白名单在**两处**，必须同改 | `MapToolsWindow.cs:151`；`IMao-Core/src/Runtime/MapToolsBridge.h:104` |
| 路线页高度 640、宽 ≤1000，由 `LayoutForGame` 给值 | `MapToolsWindow.cs:527-536` |
| 桌面路线页现在只剩：讲解卡 + 只读列表 + 「开始指引/刷新/删除」三个按钮 | `FunctionPage.xaml:20-51`；`FunctionPage.xaml.cs:54-173` |
| 桌面列表行模板：`DisplayLabel`（`●` 前缀）/`DetailLabel`/`Kinds` 图标 | `FunctionPage.xaml:25-42`；`RoutePlanningState.cs:181-192` |
| 原生 ImGui 那套路线工具栏是**死代码**，改它没有任何效果 | `DrawMarkerInteraction.cpp:650-655,656-745,760-795` |
| 页面全部文本是硬编码中文；`.resw` 只有一处 `x:Uid` 在用 | `UsageGuidePage.xaml:52`；`Helpers/ResourceExtensions.cs:7-9` |

### 3.4 文件对话框与弹窗

| 事实 | 证据 |
|---|---|
| 全仓**没有** `FileOpenPicker`/`FileSavePicker`/`FolderPicker`，也没有 `InitializeWithWindow` | 全仓 grep 为零；唯一提及是 `Docs/LegacyDataRecovery_20260927.md:182`（列为「留作下一步」） |
| 唯一的文件选择是 Win32 `GetOpenFileNameW`，带 owner HWND，**提权下也能用** | `Helpers/ResourcePackagePicker.cs:8-33,55-57`（注释 `:7` 说明提权原因） |
| `OFN_NOCHANGEDIR` 对 `GetOpenFileName` 无效，必须自己还原工作目录 | `ResourcePackagePicker.cs:29-31`；`Docs/ResourcePackagePickerFix-20260909.md:9` |
| 弹窗没有服务层，一律内联 `new ContentDialog { XamlRoot = XamlRoot, … }` | `FunctionPage.xaml.cs:151-163`（删除路线确认框）；`SettingsPage.xaml.cs:511-523,1063-1086` |
| **没有任何「A 还是 B」的复用弹窗** | 现存只有 `async Task<bool> ConfirmXxxAsync()` 这一种私有写法 |
| `SuggestionDialog.xaml` 是零引用的死模板 | 自身两个文件之外全仓无引用 |

### 3.5 IPC

| 事实 | 证据 |
|---|---|
| 命名管道，一行一个 JSON；`requestId` 关联应答；**15 秒超时** | `CoreHostService.cs:124,136-139,154-155,411-426` |
| `ExecuteRoutePlanningAsync` 返回的是 `RoutePlanningState`（由 ack 的 `data` 反序列化），**不是任意载荷** | `CoreHostService.cs:426-427`；`CoreHostMain.cpp:504-508` |
| 拒答走异常：`accepted:false` → `TrySetException(InvalidOperationException(message))` | `CoreHostService.cs:556-557` |
| 快照按 `Revision` 单调过滤，一份全局状态流 | `CoreHostService.cs:743-749`；DI 单例 `App.xaml.cs:72` |
| 原子写会在目标旁建临时文件再 `MoveFileExW` 替换，并**自动建父目录** | `AtomicFile.h:13-35` |

### 3.6 账号（记录本）与数据的耦合

| 事实 | 证据 |
|---|---|
| 路线按记录本分目录；**改记录本名字不能改 id**，否则路线与进度脱钩 | `Docs/LocalAccounts_20260926.md:187-189` |
| 删除记录本是软删除：进度 + `SavedRoutes/Auto/<id>` 一起搬进 `SavedPoints\deleted\<时间>-<id>\` | `LocalAccountCatalog.cs:333-341,389-391`；`Docs/LocalAccounts_20260926.md:413` |
| 程序包禁止包含 `SavedPoints`/`SavedRoutes`/`Logs`，所以路线包**永远不能走更新通道** | `IMao-WinUI.Core/Updates/ProgramPackageValidation.cs:55-56` |

### 3.7 现在**没有**的东西（诚实清单）

- 路线**没有**任何导出、导入（用户侧的）、合集/分组、批量操作。唯一的「导入」是启动时的一次性旧手绘迁移。
- 托管侧**从不自己枚举路线文件**，只向核心要 `list`。
- `SelectionMode="Multiple"`、路线行的 `CheckBox`、路线上的 `GroupBy` **全都不存在**。
- 合集/分组概念在代码、文档、记忆里**都不存在**。

## 4. 目标设计

### 4.1 数据模型

**合集是路线自己的一个属性，不是一个目录。** 理由：`List()` 本来就要读每个路线文件才能给出点数/类型/来源，多读一个 `collection` 字段是零成本；而改成目录会让 `List` 变成递归遍历、`Load`/`Delete`/`active.json` 全部要改，风险远大于收益。

```cpp
// RoutePlanningModel.h — AutoRoute::Plan 新增
std::string collection = "default";   // "default" 是保留 id，永远存在，不可改名/删除

// RoutePlanStore.h — 落盘新增一个字段（与 farmMode/handDrawn 同位置）
{"collection", plan.collection.empty() ? std::string{"default"} : plan.collection}
// 读：doc.value("collection","default") —— 老文件没有这个字段 ⇒ 默认合集，
//     与 farmMode 的「缺字段就是安全读法」完全同构，不需要任何迁移脚本。
```

**合集索引**（每个记录本一份，与路线同目录树，`Auto`/`Hand`/`Legacy` 平级）：

```
%LOCALAPPDATA%\IMao-WinUI\SavedRoutes\
├─ Auto\<profile>\<routeId>.json        路线（新增 collection 字段）
├─ Auto\<profile>\active.json
├─ Hand\<profile>\<routeId>.json        路线（新增 collection 字段）
├─ Collections\<profile>.json           ← 新增：合集索引 + 当前合集
└─ Legacy\

// Collections\<profile>.json
{ "formatVersion": 1,
  "current": "default",
  "collections": [ { "id": "8f3c…", "name": "宝箱路线", "createdUnixMs": 1759300000000 } ] }
```

- **默认合集不进数组**：它在两端都按 id `"default"`、名字「默认合集」现造，`deletable=false`。这样「现有的路线都归为默认合集」是**零迁移**成立的。
- 合集 id 用与路线 id 同一套校验（`ValidateRouteComponent`），另保留 `default`。
- 索引文件缺失/损坏 ⇒ 只有默认合集，`current="default"`，**不覆盖原文件**（与 `accounts.json` 的既有纪律一致）。
- `current` 随每次切换原子写；切档案时与路线一起重读。

**目录名 `Collections` 是安全的**：旧手绘导入只看顶层普通文件（`LegacyHandRouteImportFile.h:28-32`），`ProgramPackageValidation` 的保留清单里已有 `SavedRoutes`（`:55`）。

### 4.2 核心新增动作

全部走现有 `routePlanning` 通道；**一律用 `routeIds`（复数数组）而不是 `routeId`**，所以不会碰到 `:865-866` 那条活动路线围栏（`switch` 当年就是漏加豁免被静默拒绝的）。

| action | 载荷 | 行为 | 拒绝条件 |
|---|---|---|---|
| `collectionNew` | `name` | 建一个合集，并**切到它** | 名字空/超 40 字/重名 |
| `collectionRename` | `collectionId`,`name` | 改名 | `default` 不可改；重名 |
| `collectionDelete` | `collectionId` | **删掉合集中的全部路线**，再删索引项；若删的是当前合集 ⇒ 切回 `default`；若删掉的路线里有活动路线 ⇒ 先 `StopNavigationLocked()` | `default` 不可删 |
| `collectionCurrent` | `collectionId` | 切换当前合集（只写索引的 `current`） | 合集不存在 |
| `routeCollection` | `routeIds[]`,`collectionId` | 把一条或一批路线移到目标合集（单条 = 长度 1 的数组） | 目标合集不存在；路线不存在 |
| `export` | `path`,`collectionId`（导出合集）**或** `path`,`routeIds[]`（导出所选） | 写路线包文件 | 两者都没给；`path` 空；无路线 |
| `importInspect` | `path` | **只读**：解析、校验、把摘要放进快照的 `transfer`，**不改任何数据** | 文件不存在/读不动/格式不对 |
| `importApply` | `path`,`mode` ∈ `routes`/`collectionOverwrite`/`collectionNew` | 执行导入 | 没有对应的 `transfer`；文件变了且新摘要与 `mode` 不符 |

**保存落在哪个合集**（这是需求 1 的核心，判据要写死在代码里）：

| 入口 | 合集 |
|---|---|
| `activate`（预览开始指引）、`save` 且 `target=="preview"`、`handCommit` | **新路线 ⇒ `r.currentCollection`** |
| `save` 且存的是**已经在册的活动路线** | **保持该路线自己文件里的 `collection`**（不因为「当前在别的合集」就把老路线搬走） |
| `skip`/`undoSkip`/`farm` 内部重写路线文件 | 天然保持（它们 `Save(*r.active)`，字段原样带过） |

新增两个 action 的**豁免说明**：`export`/`routeCollection` 用 `routeIds`，不进 `:865` 的 `routeId` 判断；但要在代码注释里写明这一点，并加一条测试钉住（见 §7 T7）。

### 4.3 快照新增字段

```jsonc
"currentCollection": "default",
"collections": [
  { "id": "default", "name": "默认合集", "routeCount": 3, "current": true,  "system": true  },
  { "id": "8f3c…",  "name": "宝箱路线", "routeCount": 5, "current": false, "system": false }
],
"savedRoutes": [ { …原有 8 个字段…, "collection": "default" } ],
"transfer": null              // 或 importInspect 之后的摘要，见下
```

`transfer` 的形状（`importApply` 成功后清空）：

```json
{ "kind": "collection", "collectionName": "宝箱路线", "routeCount": 12, "stopCount": 340, "skipped": 0,
  "conflict": { "collectionId": "8f3c…", "name": "宝箱路线", "routeCount": 5 } }
```

托管侧对应新增 `Models/RoutePlanningState.cs`：`RouteCollection` 记录、`SavedAutomaticRoute.Collection`、
`RoutePlanningState.Collections` / `CurrentCollection` / `Transfer`。

### 4.4 路线包格式（导入导出的唯一真相）

```jsonc
{ "formatVersion": 1,
  "app": "IMao",
  "kind": "routes" | "collection",
  "exportedAtUnixMs": 1759300000000,
  "collection": { "name": "宝箱路线" },          // 仅 kind=="collection"
  "routes": [ <与磁盘上的路线文档逐字段相同的一份> ] }
```

- `routes[]` 里的每一项**就是 `RoutePlanStore::Save` 写的那份文档**（含 `id/profileId/collection`）。这样导出/导入天然可逆，且读写只有一处实现。
- 导入时**改写三个字段**：`profileId` → 当前记录本；`collection` → 目标合集；`id` → 见下。
- `id` 规则：**目标记录本里 id 没被占用就沿用**（于是「覆盖」模式下重复导入同一个包是幂等的）；**被占用就换 `NewId()`**，若名字也撞了就追加 ` (2)`、` (3)`。
- `kind` 由导出时决定：勾选导出 ⇒ `routes`；导出合集 ⇒ `collection`。**导入端只看 `kind`**，玩家不需要先选模式（D4）。
- 不含完成记录、不含攻略记录。完成记录属于**点位进度**（`SavedPoints`），跟人走不跟路线走，这与「删除路线不影响点位的完成记录」是同一条既有语义。
- 大小上限 16 MiB；单条路线仍受 `Read` 的 2 MiB 与 `Validate` 的 1..500 点约束。

### 4.5 导入流程（两步，中间那次弹窗是需求要的）

```
点「导入…」→ Win32 打开对话框选文件
   → routePlanning{action:"importInspect", path}
       核心：解析 + 逐条校验 + 写 transfer（profileId 不匹配、场景未知、点位数>500 等先记下）
       ★ 一条数据都不改
   → 界面按 transfer.kind 分流：
        kind=="routes"      → 一句确认「把 12 条路线导入当前合集「默认合集」？」            → importApply{mode:"routes"}
        kind=="collection"  → 无同名合集：一句确认「新建合集「宝箱路线」并导入 12 条路线，
                                           同时切换过去？」                              → importApply{mode:"collectionNew"}
                             有同名合集：**弹窗三选**：
                                「覆盖」= 删掉现有 5 条，换成导入的 12 条                → importApply{mode:"collectionOverwrite"}
                                「新建」= 建一个不同名的合集（宝箱路线 (2)）导入           → importApply{mode:"collectionNew"}
                                「取消」                                                  → 不发命令
```

- `importApply` 会**重新读一遍文件并重新校验**（不缓存大 JSON），若与 `transfer` 的摘要不符就以新文件为准并重新要求确认——文件在这两步之间被换掉时不至于照旧执行。
- **先全部校验、再落盘**（沿用旧数据恢复那条纪律「一半完成的恢复比不恢复更糟」）：
  - 有效路线 0 条 ⇒ **整批放弃**，一条都不改，报明原因；
  - 有部分无效 ⇒ 导入有效的，逐条把无效原因写进消息（「导入 10 条，跳过 2 条：点位资源已变化」），并写 `StructuredLogger` 的 `routes/route-bundle-import-skipped` 行。
  - 「覆盖」模式：**校验通过之后**才删旧路线，再写新的。删除用现成的 `RoutePlanStore::Delete`（自带 `.deleting` 墓碑与活动指针处理）。
- 角色互换的那个弹窗（覆盖 vs 新建）在**桌面页**用 `ContentDialog` 三按钮（`PrimaryButtonText="覆盖"`、`SecondaryButtonText="新建"`、`CloseButtonText="取消"`）。这是本仓第一个「A 还是 B」弹窗，就地写，不引入服务层。

### 4.6 界面设计

#### 4.6.1 桌面「路线」页（管理面）

```
ROUTE STUDIO / 路线
路线怎么操作，以及你保存过哪些路线。
[消息条]

01 路线怎么操作        （不动）

02 路线列表
合集：[默认合集] [宝箱路线] [采集] [全部]   …    [新建合集] [重命名] [删除合集]
说明行：正在走的路线 / 已完成 N / 当前目标
[批量] [导入…] [导出当前合集]
                                 ┌ 批量模式时出现 ─────────────────────────┐
                                 │ 已选 3 条  [全选] [导出所选] [移动到…] [删除所选] [退出批量] │
                                 └────────────────────────────────────────┘
☐ ● 路线名                    ← 批量模式时行首出现 CheckBox
     12 个点 · 璃月 · 自动 · 宝箱路线
     [图标] 宝箱  [图标] 怪物
☐ 另一条路线
…

[开始指引这条路线] [刷新列表] [删除所选路线]
```

- **合集条即「当前合集」选择器**：点某合集 = 切换当前合集（与游戏内共用同一份核心状态）。额外一个「全部」是**只读浏览**，不改变当前合集，只把列表铺开。
- 「导出当前合集」在「全部」下置灰（因为那时没有唯一的合集可导出）。
- 行上多一句所属合集；「全部」视图下尤其需要。
- 批量模式：行首 `CheckBox`。列表换成 `SelectionMode="Multiple"` 的 ListView 用不上——行的点击已经是 `switch`，勾选必须是独立控件，所以**不用 ListView 的选中**，每行一个 `CheckBox` 绑 `IsChecked`。
- 手柄不参与桌面页（现状如此，不新增）。

#### 4.6.2 游戏内工具窗「路线列表」页（操作面，只加必要的）

```
路线列表
合集：[默认合集] [宝箱路线] [采集]              ← 一行按钮，当前合集用主色
当前路线：宝箱路线 A · 12 个点 · 宝箱、怪物      ← 不动（活动路线可能不属于当前合集）
[删除这条路线] [刷新列表] [返回路线规划]
已保存的路线（当前合集 3 条 · 其它合集还有 5 条）
  ● 宝箱路线 A
  …
手绘路线
  …「保存手绘路线」按钮旁注明：将保存到「默认合集」
```

- 只加两件：**顶部合集切换条**、**保存目标的注明**。「保存这条路线」「保存手绘路线」的提示里带上当前合集名。
- 列表只列**当前合集**的路线（「当前所在合集」的字面含义）；活动路线卡片照旧在顶部，不受合集影响。
- 新按钮进 `RebuildNavigation` 的导航环（`routes` 页的 header 按钮已经在环里，合集条照同样方式加）。
- 页高可能要从 640 提到 700（`LayoutForGame`）。**页名白名单不动**（没有新页），所以 `MapToolsBridge.h:104` 不需要改。

#### 4.6.3 文件对话框

把 `Helpers/ResourcePackagePicker.cs` 重构为通用的 `Helpers/NativeFileDialog.cs`：

```csharp
internal static class NativeFileDialog {
    internal static string? Open(nint owner, string filter, string title);
    internal static string? Save(nint owner, string filter, string title, string defaultName);
}
// ResourcePackagePicker.Pick(owner) => NativeFileDialog.Open(owner, "地图资源离线包 (*.zip)\0*.zip\0\0", "选择由维护者发布的地图资源离线包")
```

- 复用同一个 `OPENFILENAMEW` 结构与 `OFN_NOCHANGEDIR` + 工作目录还原的既有教训。
- 存档对话框加 `OFN_OVERWRITEPROMPT (0x2)`。
- 路线包过滤器：`IMao 路线包 (*.json)\0*.json\0所有文件 (*.*)\0*.*\0\0`；导出默认名 `<合集名或"路线" >-<yyyyMMdd>.json`。
- 现有 `Tests/ResourcePackagePicker` 把这个结构体的大小/偏移/取消路径钉住了，**保持它继续测同一个结构体**（改指向 `NativeFileDialog`），新对话框因此白拿一半覆盖。

## 5. 改动清单（文件级）

### 原生核心

| 文件 | 改动 |
|---|---|
| `IMao-Core/src/Runtime/RoutePlanningModel.h` | `Plan` 新增 `std::string collection = "default";` |
| `IMao-Core/src/Runtime/RoutePlanStore.h` | `Save` 写 `collection`；`LoadFolder` 读（缺字段 ⇒ `"default"`）；`List` 的行带 `collection`（坏文件回落 `"default"`）；`Validate` 不校验合集存在性（那是服务的事） |
| `IMao-Core/src/Runtime/RouteCollections.h`（新） | 合集索引的读写与校验：`Load(root,profile)` / `Save` / `Default()` / `Find` / 名字去重（` (2)`）；纯逻辑、无服务依赖，可单独单测 |
| `IMao-Core/src/Runtime/RouteBundle.h`（新） | 路线包读写：`Write(collections, routes, kind)` / `Read(text)` / `Inspect`；纯逻辑，可单独单测 |
| `IMao-Core/src/Runtime/RoutePlanningService.cpp` | 新 action 全部落在这里：`:871-998` 的链上插分支；`r.collections` / `r.currentCollection` / `r.transfer` 三个状态；`SyncProfileLocked` 一并重读合集；`SnapshotLocked` 加 `currentCollection`/`collections`/`transfer`；`save`/`activate`/`handCommit` 写合集；**新增拒绝路径日志** |
| `IMao-Core/src/Runtime/RoutePlanningService.h` | `RoutePlanningView` 加 `collections` / `currentCollection` / `transfer` |
| `IMao-Core/CMakeLists.txt` | 若新头文件自成一个测试二进制，登记 `add_executable` + `add_test` |

`AtomicFile.h` / `CoreHostMain.cpp` / `MapToolsBridge.h` **不需要改**。

### 托管外壳

| 文件 | 改动 |
|---|---|
| `IMao-WinUI/Models/RoutePlanningState.cs` | `RouteCollection` 记录；`SavedAutomaticRoute.Collection`；`RoutePlanningState.Collections/CurrentCollection/Transfer` + `RouteBundleTransfer` 记录 |
| `IMao-WinUI/Helpers/NativeFileDialog.cs`（新） | `Open` / `Save` |
| `IMao-WinUI/Helpers/ResourcePackagePicker.cs` | 改为转发到 `NativeFileDialog` |
| `IMao-WinUI/Views/FunctionPage.xaml` / `.xaml.cs` | 合集条、批量模式、勾选框、导入导出按钮、三个弹窗 |
| `IMao-WinUI/Views/MapToolsWindow.cs` | `RenderRoutes` 加合集条与保存目标注明；`LayoutForGame` 页高 |
| `IMao-WinUI/Services/MapToolsController.cs` | 新增 `collection:<id>` / `collectionNew` / `collectionDelete` 的按键分发 |
| `IMao-WinUI/Services/LocalAccountCatalog.cs` | 删除记录本时把 `SavedRoutes/Collections/<id>.json` 一起搬进归档（否则合集索引变孤儿） |
| `IMao-WinUI/Views/UsageGuidePage.xaml` | 帮助页补一段「合集与导入导出」（可选，随最后一阶段） |

**不改**：`CoreHostService.cs`（复用 `ExecuteRoutePlanningAsync`）、`ProgramPackageValidation.cs`（`SavedRoutes` 已在保留清单）、`Docs/README.md` 之外的索引文件、`Version.props`。

### 文档

| 文件 | 改动 |
|---|---|
| `Docs/RouteCollectionsAndTransfer_20261001.md`（本文） | 实施时按 §6 续写 §11 |
| `Docs/README.md` | 在「地图数据、定位与校准」表登记一行（顺手把漏登记的 `RouteUiTrimAudit_20261001.md` 也补上） |
| `MEMORY.md`（不进 git） | §2 状态、§7 已拍板（D1–D6 逐字）、§6 新坑（若有） |

## 6. 实施阶段（每阶段可独立验证、可独立提交）

| 阶段 | 内容 | 独立验证 |
|---|---|---|
| **S1 存储与模型** | `Plan::collection`；Store 读写 `List` 带出；`RouteCollections.h` | `IMaoRoutePlanningTests` 新增用例全绿；老文件读成默认合集 |
| **S2 核心命令** | 5 个合集 action + `save`/`activate`/`handCommit` 落合集 + 快照三字段 | `IMaoRoutePlanningServiceTests` 新增用例；`routeIds` 围栏用例 |
| **S3 导出** | `RouteBundle.h` 的写 + `export` action | 导出文件逐字段断言；坏路线不进包 |
| **S4 导入** | `RouteBundle.h` 的读 + `importInspect`/`importApply` + 三种 mode + 先校验后落盘 | 同名覆盖/新建、部分无效跳过、0 有效整批放弃 |
| **S5 托管模型与选择器** | `RoutePlanningState` 三字段；`NativeFileDialog` + `ResourcePackagePicker` 转发 | `ManagedRuntime` 真 IPC 往返；`Tests/ResourcePackagePicker` 仍全绿 |
| **S6 桌面 UI** | 合集条、批量模式、勾选、导入导出、三个弹窗 | `Tests/RoutePageRuntime` 新增断言 + 截图；`IMao-WinUI.csproj` Release/x64 零错误 |
| **S7 游戏内 UI** | 合集切换条、保存目标注明、页高 | `Tests/MapToolsRuntime` 新增断言 |
| **S8 收尾** | 文档 §11、`Docs/README.md`、`MEMORY.md`、门禁跑绿 | `scripts\Test-Runtime.ps1` 退出码 0 |
| **真机验收（用户本人）** | 见下 | AI 做不到：`IMao-WinUI.exe` 需要提权 |

**真机验收清单（交给用户）**
1. 游戏内保存一条自动路线 → 桌面看到它落在「默认合集」。
2. 桌面新建合集「测试」→ 游戏内列表切到「测试」→ 保存一条手绘路线 → 它落在「测试」。
3. 把一条路线从「默认合集」移到「测试」；两边列表都跟着变。
4. 勾选 3 条 → 导出 → 删掉其中 1 条 → 导入 → 3 条都在，无重名冲突。
5. 导出「测试」合集 → 导入 → 同名弹窗出现 → 选「覆盖」→ 数量正确；再导入一次 → 正常（幂等）。选「新建」→ 出现「测试 (2)」。
6. 删除合集「测试」→ 里面的路线一起消失，活动路线若在其中则退出导航，当前合集回到「默认合集」。

## 7. 测试计划

**原生（`IMao-Core/tests/`）**

| # | 用例（英文全句，与既有风格一致） | 落在 |
|---|---|---|
| T1 | `a route saved into a collection round-trips through save and load` | `RoutePlanningTests.cpp` |
| T2 | `a route file without a collection field reads as the default collection` | `RoutePlanningTests.cpp`（老数据兼容） |
| T3 | `the collections index round-trips and a corrupt index falls back to the default collection only` | `RoutePlanningTests.cpp` |
| T4 | `listing a profile reports each route's collection` | `RoutePlanningTests.cpp` |
| T5 | `saving a preview and committing a hand drawing both land in the current collection` | `RoutePlanningServiceTests.cpp` |
| T6 | `saving an already-saved route keeps its own collection instead of the current one` | `RoutePlanningServiceTests.cpp` |
| T7 | **`batch actions carry routeIds, so the active-route id fence never refuses them`** | `RoutePlanningServiceTests.cpp`（对照 `:865-866`；把 `routeIds` 改回 `routeId` 必须变红） |
| T8 | `deleting a collection deletes its routes and stops navigation when the active route was one of them` | `RoutePlanningServiceTests.cpp` |
| T9 | `the default collection can never be renamed or deleted` | `RoutePlanningServiceTests.cpp` |
| T10 | `moving a batch of routes rewrites their files and nothing else` | `RoutePlanningServiceTests.cpp` |
| T11 | `an exported bundle of a collection names the collection and holds every route verbatim` | `RoutePlanningServiceTests.cpp` |
| T12 | `importing a bundle of routes puts them in the current collection and rewrites the profile` | `RoutePlanningServiceTests.cpp` |
| T13 | `importing a collection with a taken name replaces it only after every route validated` | `RoutePlanningServiceTests.cpp` |
| T14 | `a bundle whose routes are all invalid changes nothing` | `RoutePlanningServiceTests.cpp` |
| T15 | `importing the same bundle twice is idempotent in the overwrite branch` | `RoutePlanningServiceTests.cpp` |

**托管（`Tests/ManagedRuntime/RoutePlanningTests.cs`，真 IPC + 真 CoreHost）**

| # | 用例 |
|---|---|
| T16 | 导出→改档案→导入→断言 id/名字/点数/类型在两端一致（走真实管道） |
| T17 | 合集索引文件在真实 `%LOCALAPPDATA%` 下按记录本分开，切记录本后列表跟着换 |

**界面夹具**

| # | 用例 | 落在 |
|---|---|---|
| T18 | 点「批量」后每行出现 `CheckBox`；勾 3 条后「导出所选」发出的命令带**恰好那 3 个 routeIds** | `RoutePageRuntime` |
| T19 | 合集条点某合集发出的 `collectionCurrent` 带正确 id；「全部」按钮**不**发命令 | `RoutePageRuntime` |
| T20 | 同名合集导入时出现三按钮弹窗，「覆盖」/「新建」分别发出正确的 `mode` | `RoutePageRuntime` |
| T21 | 游戏内 `routes` 页出现合集条，当前合集按钮带走主色；「保存手绘路线」旁注明当前合集名 | `MapToolsRuntime` |

**门禁**：新二进制必须进 `scripts/Test-Runtime.ps1`（`IMaoLayeredMapTests` 那条教训：**只注册进 ctest 的套件不会跑**）。`Tests/RoutePageRuntime` 与 `Tests/MapToolsRuntime` 目前**不在门禁里**，只能手动构建运行——本轮按现状手动跑并留证据，并在 §11 记一笔这个缺口。

## 8. 风险与对策

| # | 风险 | 对策 |
|---|---|---|
| R1 | 新 action 撞上 `routeId` 活动路线围栏被静默拒绝（`switch` 曾经就这样） | 一律用 `routeIds`；T7 用对照实验钉住；拒绝路径补 `StructuredLogger` 行 |
| R2 | 删合集连带删路线，误删不可恢复 | 确认框写明**条数与名字**；`RoutePlanStore::Delete` 的墓碑语义保持不变；需求方已明确选择这个语义（D2） |
| R3 | 「覆盖」导入把玩家本地路线冲掉 | 先全校验再删（0 有效则整批放弃）；确认框写「将删除现有 5 条，替换为导入的 12 条」 |
| R4 | 坏文件读不出合集归属 | 坏文件一律算**默认合集**（唯一安全读法），列表里照旧可见可删 |
| R5 | 记录本被删后合集索引成孤儿 | `LocalAccountCatalog` 一并归档；归档失败时按既有纪律报错而不是静默 |
| R6 | 桌面切合集顺带改了游戏内保存目标，玩家没预期 | 两处都显示当前合集；「全部」是只读浏览、不改变当前合集 |
| R7 | 路线包过大撑爆 IPC | 走**路径**不走载荷；包上限 16 MiB；15 秒超时对几十条路线绰绰有余（写盘是毫秒级） |
| R8 | 两步导入之间文件被换掉 | `importApply` 重读重校验；摘要不符就按新文件重新要求确认 |
| R9 | 切档案发生在导入中途 | 命令自带 `profileId` 前置围栏，核心会直接拒（`RoutePlanningService.cpp:857`） |
| R10 | 桌面页批量勾选与「行点击=开始指引」打架 | 勾选是独立 `CheckBox`，不动 ListView 的选中语义；批量模式下行的 `switch` 点击先禁用，避免误触 |
| R11 | `SavedRoutes/Hand/<profile>` 在删记录本时不被归档（**既有缺口**，非本轮引入） | 本轮只补 `Collections`；把 `Hand` 这条记进 §11 与 `Docs/OpenWork.md`，不顺手扩大改动面 |
| R12 | 新增 UI 文本是硬编码中文 | 与全仓现状一致（`.resw` 只有一处 `x:Uid` 在用），不引入新的本地化管线 |

## 9. 明确不做

- 路线包**不带**点位完成记录与攻略记录（那是 `SavedPoints` 的点位进度，跟记录本走）。
- 不做云同步、分享链接、二维码。
- 不做合集排序（拖拽）、嵌套合集、合集颜色/图标。
- 不做导入预览列表（只在弹窗里报数量；`transfer` 已带足够信息，将来想加是纯 UI 活）。
- 不动 `Version.props`（等发话）。
- 不写数据迁移脚本（老文件缺 `collection` 就天然是默认合集）。
- 不复活原生 ImGui 那套死工具栏（`DrawMarkerInteraction.cpp:650-655`）。

## 10. 我按推荐默认走的小决定（有异议请直接说）

| # | 决定 | 理由 |
|---|---|---|
| d1 | 合集是**路线文档里的一个字段**，不是目录 | `List()` 本来就读每个文件，零额外 I/O；改目录要动 `List`/`Load`/`Delete`/`active.json`，风险大得多 |
| d2 | 默认合集 id 固定为 `default`，**不进索引数组**，两端现造 | 「现有路线归默认合集」零迁移；D6 的「不可改名/删除」天然成立 |
| d3 | 包扩展名用 `.json` | 便于分享与人工查看；过滤器写明「IMao 路线包」即可区分 |
| d4 | 导入时 **id 没占用就沿用**，占用才换 `NewId()` | 「覆盖」模式下重复导入同一个包变成幂等；跨机器 UUID 撞车概率可忽略 |
| d5 | 名字撞车追加 ` (2)`、` (3)` | 与合集重名同一套规则，玩家看得懂 |
| d6 | 游戏内列表**只列当前合集**的路线 | 「当前所在合集」的字面含义；活动路线卡片不受影响，仍在顶部 |
| d7 | 桌面页额外给一个「全部」浏览项（只读） | 管理面需要一眼看全；它不是「当前合集」，所以不改保存目标 |
| d8 | 记录本删除时把 `Collections/<id>.json` 一起归档 | 否则删档后留下孤儿索引 |
| d9 | `ResourcePackagePicker` 重构为 `NativeFileDialog` 的转发 | 复用同一份 `OPENFILENAMEW` 与既有教训，且现有测试项目继续覆盖同一个结构体 |

## 11. 实施记录

（待实施，按 §6 逐阶段续写。）
