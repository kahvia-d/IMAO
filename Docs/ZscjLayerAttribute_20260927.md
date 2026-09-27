# 终声残卷在星炬学院不显示：核实报告（2026-09-27，分支 `investigate/zscj-layer-attribute`）

玩家反馈：**终声残卷在地图上无法显示**，玩家位置在**星炬学院**这类分层地图里。
用户假设：*终声残卷这类物品没有层级属性，所以被 IMAO 当成"分层地图以外的收集点位"从而隐藏。*

**核实结论：假设的方向对，机制要说准一句——不是"被当成分层地图以外的点位"，而是
"没有层级属性的点被当成**地表**点，而地表点在玩家被判定站在分层楼层上时会被隐藏"。**
两点都由本次实测钉住：

1. **数据**：拉海洛（frame 906）的 **70 个终声残卷点，0 个带层级属性**（`floorId=""`、`level="0"`）。
   同一地点同一上游，别的类型都带（学院里 基准奇藏箱 10 点、小型信标 5 点、朴素奇藏箱/共鸣医疗科/
   载具系泊场/车手一号 各 1 点）。
2. **代码**：`LayeredMap::RoleFor` 把"没有层级"的点判成 `SurfaceRole`，即
   `sharedGround ? Normal : Hidden`；星炬学院的分层**永远不会**进入 `sharedGround`
   （它的楼层美术 58%–73% 是照着地表画的，被 `ArtIsSurfaceCopy` 挡掉）。

---

## 一、证据 A：数据（本对话实测）

点数统计（`IMao-Core/src/Resource/itemsData_Lahai.json`，与上游快照
`Assets/KuroMap/states/state-906.json` 逐点一致）：

| | 点数 |
|---|---:|
| 拉海洛总点数 | 2536 |
| 带层级（`floorId` 非空且 `level` 是真实楼层） | 112 |
| **不带层级（`floorId=""`、`level="0"`）** | **2424** |
| 其中 **终声残卷（`id=zscj`）** | **70 / 70 全部不带层级** |

上游现在（本对话 `curl` 直取，未改仓库）：

```
curl.exe -sI https://web-static.kurobbs.com/mcmap/position/906/position.json
  Content-Length: 625369   Last-Modified: Sun, 20 Sep 2026 13:30:02 GMT
upstream sha256 = 22143452337728171c7d68f5dcc761f041d6eea5dbf21960a2cb208853c5520f
local    sha256 = 76481ac43d69064e4f422a1f199d543d200510bb7516497de7a74a159059db2e
```

上游今天的 zscj 只有 **2 个**点带层级，都在 `-1/37`（联坠长廊·基座段）；
**星炬学院的 9 个点一个都没有层级**。所以这不是"我们的同步丢字段"：
`Sync-KuroMapData.ps1` 原样保留 `floorId`/`level`（`Docs/KuroMapDataSource.md` 第 5 行）。
下游读取链也没有丢：`DrawItemBase::AddItemDataFromJson` 直接用
`location["floorId"]` / `location["level"]` 填 `ItemDatas::layer`（`DrawItemBase.cpp:273-274`）。

### 学院里的点，坐标落在学院楼层的足迹里

分层足迹 = `Assets/FeaturesDatas/KuroTilePacks/lahai/layered-floors/floor-index.json`
的 occupancy 网格。用运行时同一套换算（`LayeredFloorIndex.cpp` `LocateCell`/`Contains`，
点数据是"游戏单位的百分之一"，即 `mapX = raw/100 * scale + origin`）复算：

| 学院楼层 | 覆盖的 zscj 点数 |
|---|---:|
| `-1/30` 星炬学院·广场区 | 9 |
| `-2/30` 星炬学院·教学区 | 4 |
| `-3/30` 星炬学院·运载区 | 2 |
| `-4/30` 文献中心·休憩区 | 1 |

（学院是层叠建筑，四个楼层覆盖同一片坐标，所以同一坐标会同时落在多层里。）
全部 70 个 zscj 点里 **22 个**落在拉海洛某个分层足迹内。

**玩家最可能遇到的那个点**（描述就写着学院）：

```
x=-64000 y=-537700  "在星炬学院·教学区的最顶部。可以从图2所示传送点沿路向终声残卷所在地方前进…"
  floorId="" level="0"
  落在：-1/30 广场区 / -2/30 教学区 / -3/30 运载区 / -4/30 文献中心·休憩区
```

同一片区域里，上游给 **基准奇藏箱** 的层级是 `30` / `-2/30`（教学区）——也就是说，
"教学区"这层在上游数据里是存在的，只是终声残卷这类没有被标上去。

> 换算校验（防止我用错坐标系）：上游**确实标了层级**的 104 个点里，
> **100 个**用上面这套换算落在它自己的楼层足迹内；如果把点数据直接当成运行时
> ImgMap 坐标去算，只剩 55/104。前者才是对的。

---

## 二、证据 B：代码（分层内隐藏地表点）

`IMao-Core/src/Runtime/LayeredMapState.cpp`：

```cpp
// 582-584
MarkerRole SurfaceRole(const Snapshot& state) {
    return state.sharedGround ? MarkerRole::Normal : MarkerRole::Hidden;
}
// 588-605  RoleFor
if (item.layer.stateId != 0 && state.kuroStateId != 0 && item.layer.stateId != state.kuroStateId)
    return MarkerRole::Normal;                       // 别的 Kuro state 不归它管
const auto& mapId   = item.layer.floorId;            // "30"      = 分层地图 id
const auto& floorId = item.layer.level;              // "-2/30"   = 层
if (mapId.empty() || floorId.empty()) return SurfaceRole(state);   // 没有层级 → 地表点
const int level = LayeredFloors::FloorLevel(floorId);
if (level == 0 || level <= -1000000) return SurfaceRole(state);    // level "0" / 入口点 → 地表点
```

`Hidden` 的后果（四处，全部跳过绘制/交互）：

| 位置 | 行为 |
|---|---|
| `ImguiDraw/Items/DrawMarkerInteraction.cpp:1219` | 大地图**不画** |
| `ImguiDraw/Items/DrawMarkerInteraction.cpp:1348-1349` | 不进布局 → **不可悬停、不可点选、不参与成组** |
| `ImguiDraw/Items/DrawItemOnMinMap.cpp:282-283` | 小地图**不画** |
| `Runtime/NearbySelection.h:64-65` | 附近收集/攻略的按键**不作用于它** |

### 星炬学院为什么连"共用地表"的放行都没有

`LayeredFloors::SharesSurfaceGround`（`Feature/LayeredFloorIndex.cpp:348-362`）
= **临地表层**（`level == -1`）**且** 该层美术不是照着地表画的（`copiedFraction < 0.5`）。
后者是 2026-09-23 为星炬学院专门加的（`Docs/LayeredMapFeaturePackPlan.md` §"共用地表判定"）：
当时站在学院广场区，比对报 `shared=1 fraction=1.0`，把学院外的地表标记全放了出来，
而"那层的地面是学院自己的，不是地表的"。

本对话从**随包下发的** `floor-index.json` 逐位复算 `copiedFraction`（shared 位 / occupancy 位）：

| 楼层 | copiedFraction | `SharesSurfaceGround` |
|---|---:|---|
| `-1/30` 星炬学院·广场区 | **0.733** | ✗ 被 `ArtIsSurfaceCopy` 挡掉 |
| `-2/30` 星炬学院·教学区 | 0.675 | ✗（且不是临地表层） |
| `-3/30` 星炬学院·运载区 | 0.582 | ✗ |
| `-4/30` 文献中心·休憩区 | 0.077 | ✗（不是临地表层） |
| 对比：`-1/15` 下层金库·贵金属与艺术品藏区1楼 | 0.272 | ✓ 会放行 |

所以 **在星炬学院的任何一层上，地表点都是无条件隐藏的**，没有"共用地表"这条退路。
行为本身是**有意为之并且已有回归测试**：`IMao-Core/tests/LayeredMapTests.cpp:138`
（`""`/`""` → Hidden）、`:150`（`"1"`/`"0"` → Hidden）。

---

## 三、影响面：不是只有终声残卷

按 `RoleFor` 语义在真实数据上逐点复算（玩家被判定在星炬学院某层、`kuroState=906`、
`sharedGround=false`；拉海洛共 2536 点）：

| 玩家所在层 | `Hidden`（地表点） | `Hidden`（属于别的分层） | `Current` | `Above` | `Below` | 终声残卷被隐藏 |
|---|---:|---:|---:|---:|---:|---:|
| `-1/30` 广场区 | 2432 (95.9%) | 85 (3.4%) | 10 | 9 | 0 | **70 / 70** |
| `-2/30` 教学区 | 2432 (95.9%) | 85 (3.4%) | 4 | 5 | 10 | **70 / 70** |
| （对比）`-1/37` 联坠长廊·基座段 | 2432 (95.9%) | 57 (2.2%) | 47 | 0 | 0 | **70 / 70** |

- 站在 `-1/30`（广场区）：**70 / 70 个终声残卷全部隐藏**；
- 站在 `-2/30`（教学区）：同样 70 / 70 隐藏。

也就是说，玩家在星炬学院里看到的是"**拉海洛 99% 的点位消失**"，终声残卷只是其中一类
（学院自己的宝箱/信标因为带层级而照常显示，所以对比之下"就这个物品不见了"特别显眼）。
玩家之所以只报终声残卷，很可能就是因为他正在学院里找这一类。

### 顺带发现（同一族问题）

全游戏还有 **9 个点"有分层 id、但 level 是 `0`"**，它们同样被读成地表点、同样在分层内隐藏：

```
itemsData_World.json        破霜猎手 ×2 (floorId=3)  先锋幼岩 ×1 (1)  叮咚咚 ×2 (1)  极寒叮咚咚 ×1 (1)
itemsData_LowerVault.json   气动棱镜 ×3 (floorId=17)
```

---

## 四、还没证实的两点（需要反馈者/你的输入）

1. **反馈者当时的分层状态是不是真的 active？**
   只有 active 才会隐藏地表点。本机日志里**没有任何一次拉海洛的分层采纳**：
   `events-2026092{4,5,6,7}.jsonl` 的 `layered-floor-change` 全是 `scene=6`（下层金库），
   `scene=5` 一条都没有。要钉死这一点，需要他那一刻的 `layered-floor` / `layered-floor-change` 行
   （或者按第五节的探针，用一张学院里的实机截图复现采纳）。
2. **上游为什么不给星炬学院的终声残卷标层级？** 两种解释都与数据一致：
   - 这些点在地表图上也能看见（屋顶/广场/半空），上游把它们算作地表点；
   - 或上游 906 的这类收集道具覆盖不全（同一区域内其它类型都有层级）。

   这决定修法走"补数据"还是"改行为"，所以不替你判定。

---

## 五、候选修法（未实施，等你拍板）

| | 做法 | 代价 / 风险 |
|---|---|---|
| **A** | **补数据**：给学院里那 9 个（至少被反馈的那 1 个）zscj 点补 `floorId`/`level`，例如 `(-64000,-537700) → 30 / -2/30` | 需要游戏事实确认每一层；上游同步会覆盖，要放进补丁层或给同步脚本加白名单；不改任何行为，风险最小 |
| **B** | **改行为**：把"美术是照着地表画的楼层"（`ArtIsSurfaceCopy`：学院 4 层、日树 42/43）也当作共用地表放行 | 与 2026-09-23 的决定相反——当时正是这个放行让学院外的地表标记全跑出来 |
| **C** | **改行为 2**：只对"坐标落在当前楼层足迹内"的地表点保留显示（暗色/上下层样式） | 现在隐藏的判据是"层"而不是"位置"，这是设计规则的改变，需要你明确要 |
| **D** | **不动**：若确认反馈者其实在学院外、或日志显示当时没有 active 的分层状态 | — |

我的意见（不代替你决定）：先按 **A** 做最小范围的数据补丁，同时把第五节的探针跑一遍
确认学院里确实会采纳楼层；B/C 会动到你已经拍过板的规则，除非你重新拍板，不碰。

---

## 七、后续（2026-09-27 已实施）

用户看过本报告后的判断：**"只有星炬学院这个地方比较特殊"**，并给出了开放/封闭分层地图的划分——
星炬学院属于**开放分层地图**（不封闭、无限定出入口、地表本身也算一层），其余都是**封闭**。
规则与实现见 **`Docs/LayeredMapOpenness_20260927.md`**。

它比本报告第五节里的 B（放宽 `ArtIsSurfaceCopy`）更准确：B 会连**第三/第四日树**一起放行，
而那两棵是**有入口标记的封闭分层**。按开放/封闭分类后，全游戏只有星炬学院那四层带
`"surfaceAccess": "open"`，其余 86 层都是 `"enclosed"`。

---

## 六、复现命令

```powershell
# 1) 全游戏分层点位覆盖率审计（本分支新增）
pwsh -File scripts\Get-LayeredMarkerCoverage.ps1 -Region lahai
pwsh -File scripts\Get-LayeredMarkerCoverage.ps1 -Item 终声残卷

# 2) 上游数据是否已经补上层级（不改仓库）
curl.exe -s --fail --location --max-time 60 `
  "https://web-static.kurobbs.com/mcmap/position/906/position.json" --output $env:TEMP\kuro-906.json
$d = Get-Content $env:TEMP\kuro-906.json -Raw | ConvertFrom-Json
$d | Where-Object { $_.id -eq 'zscj' } | ForEach-Object { $_.location } |
  Group-Object { "floorId='$($_.floorId)' level='$($_.level)'" } | Select-Object Count,Name

# 3) 既有行为回归（分层内隐藏地表点）
x64\Release\IMaoLayeredMapTests.exe

# 4) 学院里会不会真的采纳某个楼层（需要一张学院内的实机截图）
x64\Release\IMaoLayeredFloorProbe.exe --pack Assets\FeaturesDatas\KuroTilePacks\lahai `
  --reference <学院内的全屏截图.png> --full-snapshot
```

覆盖率的判据、`copiedFraction` 与 `sharedGround` 的关系都写在
`scripts/Get-LayeredMarkerCoverage.ps1` 头部注释里。
