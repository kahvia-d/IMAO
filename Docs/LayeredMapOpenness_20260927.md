# 开放分层地图 / 封闭分层地图（设计决定，2026-09-27）

起因：玩家反馈**终声残卷在星炬学院不显示**。核实过程与证据见
`Docs/ZscjLayerAttribute_20260927.md`；这里是据此定下的规则与实现。

## 一、问题

运行时的分层规则原先把**所有**分层地图都当成"进去以后地表就不算数了"的地方：

- 玩家被判定站在某个分层楼层上 → 所有**没有层级属性**的点（地表收集物、分层入口）全部隐藏；
- 属于**别的分层地图**的点也隐藏；
- 只有当前楼层、以及同一分层地图的上下层照常显示。

这对洞穴/地下/建筑内部是对的。但**星炬学院是露天多层建筑**：它不封闭、没有限定的出入口，
**广场区就是地表**。玩家站在学院里，地图上却看不到学院里那些没有层级属性的收集物
（70 个终声残卷全部如此，因为它上游没有层级），于是表现为"这个物品没了"。

## 二、定义（用户 2026-09-27 拍板）

| | 定义 | 例子 |
|---|---|---|
| **开放分层地图** | 本身不封闭，**无限定出入口**，**地表本身也算一层** | 星炬学院（全游戏唯一一个） |
| **封闭分层地图** | 本身封闭，出入口数量有限，地表不算一层 | 下层金库、眠龙庭、虚妄摇篮、落辉长隧…… |

将来可能出现**混合型**（地面开放 + 地面下封闭），所以标记的**粒度是"层"**：
默认跟随所属分层地图，单独一层可以另标。

## 三、怎么判定（两条可测信号 + 一张游戏事实表）

数据从不说"这里有没有墙"，所以类别由两个测量推出，并且允许用**游戏事实表**覆盖：

| 信号 | 含义 | 星炬学院 | 最接近的反例 |
|---|---|---|---|
| `copiedFraction >= 0.5`（临地表层 `-1`） | 该层的地面是照着**地表自己的像素**画的 ⟹ 地表本身算一层 | **0.733** | 眶折谷·本段 0.16（无入口层里最高） |
| 该分层**没有** `分层入口`(FCRK) 标记 | 没有"要进的门" | 无 | 第三/第四日树抄图 0.51 / 0.80，但**有**入口标记 ⟹ 封闭 |

两条一起用：**无入口** + **地面层抄图 ≥0.5**。2026-09-27 对全游戏 57 个分层地图 / 90 层实测，
**恰好只挑出星炬学院一个**（拉海洛 layer 30 的四层），与玩家在游戏里看到的一致。

覆盖表（`scripts/LayeredSurfaceAccess.ps1`）：

- `$LayeredSurfaceAccessLayerFacts`：`region + layerId → open/enclosed`，星炬学院一行，写清理由；
- `$LayeredSurfaceAccessFloorFacts`：`region + floorId → open/enclosed`，目前为空，留给混合型；
- 事实优先于推导；推导与事实不一致时脚本会打印出来供人复核。

## 四、运行时规则（用户确认后的语义）

判据只有两条：

1. `SurfaceRole`（"这是地表点"的点怎么画）= `sharedGround || openToSurface ? Normal : Hidden`；
2. 判断一个点算不算**地表点**时，把**开放分层地图的点**也算进去。

由此得到完整的行为表（`LayeredMap::RoleFor`）：

| 玩家所在 | 点的归属 | 行为 |
|---|---|---|
| 开放层 | 没有层级属性（地表收集物 / 分层入口） | **Normal**（本次修复的目标） |
| 开放层 | 同一开放分层的**其它楼层** | **Above / Below**（变暗 + 上下箭头，保持原样） |
| 开放层 | **别的封闭分层**的点 | **Normal**（玩家站在露天，地图就该像地表图） |
| 开放层 | 别的开放分层的点（将来） | Normal |
| 封闭层 | 没有层级属性 | Hidden（除非共用地表，见下） |
| 封闭层 | 同一分层的其它楼层 | Above / Below |
| 封闭层 | 别的封闭分层的点 | Hidden |
| 封闭层 | **开放分层**的点 | **Hidden**（它已经被定义成"地表点"，被封闭层藏掉才自洽） |

`sharedGround`（下层金库门前广场那套逐帧影像比对）**保持不动**：封闭分层里它仍然能让地表标记
在"抄来的地面"上恢复显示；开放分层的点位在那种地面上也一样算地表点。

## 五、数据与实现

- **字段**：`layered-floors/floor-index.json` 每个 floor 增加
  `"surfaceAccess": "open" | "enclosed"`（另附 `surfaceAccessReason` 说明依据）。
  **缺省 = enclosed** ⟹ 老包行为逐字不变。
  未知取值会被 `LayeredFloors::Load` **拒绝**（而不是当成 enclosed），因为把笔误静默读成封闭
  正是"标记又被藏起来"的故障形态。
- **运行时**：`FloorEntry::openToSurface`（`Feature/LayeredFloorIndex.h`）→
  `Snapshot::openToSurface`（`Runtime/LayeredMapState.h`，随楼层采纳一起设置）→
  `RoleFor`。诊断行里新增 `open=0/1`（`layered-floor` 与 `layered-floor-change`）。
- **谁写字段**：
  - `scripts/LayeredSurfaceAccess.ps1` —— 共用判定（两条信号 + 事实表）；
  - `scripts/New-LayeredFloorIndex.ps1` —— 生成索引时写入（新包自动带）；
  - `scripts/Set-LayeredSurfaceAccess.ps1` —— 给**已存在**的索引补写（`-Report` 只报告）。
    它同时校验"推导出的开放集合"等于记录下来的期望集合，数据一变就报错。
- **谁校验**：`scripts/Test-KuroMapFeaturePack.ps1` 要求每个 floor 都有合法的 `surfaceAccess`，
  并且**只有记录在案的开放分层才能是 open**（防止重新生成包时静默多出一个开放地图）。

## 六、这么改不动什么

- 分层识别（投票、own-art 否决、阈值、去抖）不变；
- 分层完成判定（"站在 1 楼不能勾掉 4 楼的箱子"）不变，同层其它楼层仍进不了完成候选；
- `sharedGround` 与 `ArtIsSurfaceCopy` 不变（第三/第四日树仍然不会被放行）；
- 冷启动检索范围、路线规划（`AutoRoute::IsSurfaceTarget` 本来就按"有没有 floorId"算）不变。

## 七、附带收益

星炬学院是**分类最不稳**的地方（四个楼层美术 58%–84% 抄地表）。改完之后：

- 认错楼层只值"变暗 + 箭头画错"，**不再值"整片地图消失"**；
- 认不出来（状态不 active）完全没有代价。

2026-09-26 全游戏扫描里"拉海洛地表样本误判进层 2.8%"那一类，从此不再有害。

## 八、验证

```powershell
# 1) 判定与开放集合（只读）
pwsh -File scripts\Set-LayeredSurfaceAccess.ps1 -Report

# 2) 补写已发布的索引
pwsh -File scripts\Set-LayeredSurfaceAccess.ps1

# 3) 每个带分层的包都要通过
Get-ChildItem Assets\FeaturesDatas\KuroTilePacks -Directory | ForEach-Object {
    if (Test-Path (Join-Path $_.FullName 'layered-floors/floor-index.json')) {
        pwsh -File scripts\Test-KuroMapFeaturePack.ps1 -PackRoot $_.FullName } }

# 4) 角色判定（开放/封闭两个方向）
x64\Release\IMaoLayeredMapTests.exe
```

真机确认：进入星炬学院后 `events-*.jsonl` 里应出现
`layered-floor-change ... floor=-1/30 ... open=1`，并且终声残卷的图标回来。
