# 隐海试验场的分层地图识别不出来的根因：分层楼层索引的坐标系建错了

- 日期：2026-09-26
- 触发：真机测试中站在隐海试验场（`fabricatorium`，frame 905）蓄能中枢下层，日志里楼层**识别成功**却
  一次都没有被采纳（分层地图等于不可用）
- 结论：`layered-floors/floor-index.json` 的 `coordinateTransform` 写的是 World 的 `(2474,1957)`，
  而 frame 905 的坐标系是 `(7437,13783)`。运行时用索引里这个变换把玩家的地图坐标换算成 tile，
  于是永远落在别的 tile 上，`Contains` 永远 false → `winnerContained=0` → 再准的楼层识别也永远不采纳。
- 这个 bug 在 `main` 上就存在（`git show main:...layered-floors/floor-index.json` 里 `originX` 就是
  `2474.0`），不是本分支引入的。

---

## 一、日志证据

`%LOCALAPPDATA%\IMao-WinUI\Logs\events-20260926.jsonl`，21:31:40 – 21:34:55，`layered-floor` 行：

```
scene=3 active=0 floor=- restricted=0 containing=[] identified=1 ownDominant=1 ownFloor=-1/22
adopted=0 winnerContained=0 decisiveStreak=0 closeStreak=0 equivalent=[]
winner=60 runnerUp=0 ownWinner=51 ownShare=0.850000 ownRunnerUp=0
votes=[fabricatorium:-1/22=60] ownVotes=[fabricatorium:-1/22=51]
```

| 量 | 一段区间内的取值 | 说明 |
|---|---|---|
| `identified` | 1（每一帧） | 楼层识别成功 |
| `votes` | 只有 `fabricatorium:-1/22`，7–62 票 | 场景里只有这一层候选，且唯一胜者 |
| `runnerUp` | 0 | 没有第二层 |
| `ownDominant` / `ownShare` | 1 / 0.33–0.89 | own-art 否决通过（就是本分支新加的那道） |
| **`winnerContained`** | **0（每一帧）** | **玩家位置不在胜者的 footprint 里** |
| `containing` | `[]` | 场景内**没有任何**一层的 footprint 包含玩家 |
| `adopted` | 0 | 因此不采纳；`active=0 floor=-` |

即：`adopted = identified && winnerContained && ownDominant` 里，前两项与第三项都过了，
**只有 footprint 包含判定是 false**——而这与 own-art、阈值、去抖都无关。

## 二、算术复现

同一帧另外两行给了我们所需的全部输入：

```
visual-local-tracking  scene=3 map=7181.019882,13140.076701   ← 运行时内部的 ImgMap 坐标
minimap-near-items     scene=3 sceneData=1 markers=1 playerROC=-255.98,642.92
```

`LayeredFloors::LocateCell`（`IMao-Core/src/Feature/LayeredFloorIndex.cpp:427`）做的正是
`game = (map − origin) / scale → tile → pixel → cell`，所以拿两个 origin 各算一遍：

| origin | game 坐标 | tile | 结果 |
|---|---|---|---|
| 索引里写的 `(2474,1957)` | `(3906.2, 9280.6)` | **(5,−10)** | 该层只有 (0,1)(0,2)(1,1)(1,2) 四块 → `tile == nullptr` → `Contains=false` |
| frame 905 真正的 `(7437,13783)` | **`(−212.4, −533.6)`** | **(0,1)** | 该层自己的 tile，cell (48,23)，occupancy 那一位 = **1** → **`Contains=true`** |

而 `(−212.4,−533.6)` 与截图左下角游戏显示的 **`−211,−532`** 只差 1.5 个游戏单位（HUD 取整），
这是独立于日志的第二份证据：frame 905 的坐标系确实是 `(7437,13783)`。

`(7437,13783)` 的来源也是三重确认的：

- `IMao-Core/src/Coordinate/CoordinateStruct.h:86` — `{ 3, "Fabricatorium", 905, 7437.0, 13783.0, 1.205, false }`，
  这是运行时自己用的编译期表；
- `IMao-Core/src/Feature/VisualIndex/MapVisualIndex.cpp:255` — `std::tuple{3, 7437.0f, 13783.0f}`；
- 包的 `manifest.json` — `coordinateTransform` 同值，且 `referenceVerification.passed = true`
  （`errorPixels 2.31`，对着一张真机截图校验过）。

## 三、坐标系是怎么写错的

`scripts/New-LayeredFloorIndex.ps1` 原来把 World 的坐标写死成默认值，只有
`Assets/KuroMap/scene-calibrations.json` 里有该场景条目时才覆盖：

```powershell
$originX = 2474.0; $originY = 1957.0; $scale = 1.205
$calibration = $calibrations.scenes.PSObject.Properties[$scene]
if ($null -ne $calibration) { ...覆盖... }
else { Write-Host "  no calibration for scene '$scene'; using the World transform" }
```

而 `scene-calibrations.json` 里只有 Tethys / Darkplain / Lahai / TimeRiftRuins / LowerVault，
**没有 Fabricatorium**（区域注册里它是 `tileConfidence: uncalibrated`、`originEvidence: null`、
`transformSource: "builtin"`）。于是整个索引的 footprint 网格都建在 World 的坐标系里，而 frame 905
的偏移是

```
Δ = (7437−2474, 13783−1957) = (4963, 11826) map px = (4118.7, 9814.1) 游戏单位 ≈ 4.9 / 11.5 个 tile
```

同一次事故的早期版本（下层金库）在脚本注释里已经记过：*"Hardcoding World's numbers made every
non-World index test containment against the wrong point, so no floor ever contained the player there"*——
当时的修法是往 `scene-calibrations.json` 里补一条校准；隐海试验场没有被补上，同样的错误就留在了这里。

十个已发布分层包的对照（`floor-index.json` vs 包自己的 `manifest.json`）：

| 区域 | frame | 索引里的 origin | manifest 的 origin | |
|---|---|---|---|---|
| jinzhou / laguna / mengzhou / qiqiu / roysurface | 8 | 2474,1957 | 2474,1957 | 一致 |
| darkplain | 909 | −0.437,0.238 | 0,0 | 差 0.1 px，无影响 |
| lowervault | 902 | −3.52,−2.46 | 0,0 | 差 4 px，无影响 |
| lahai | 906 | 21662.08,13138.69 | 21662,13138 | 差 0.08/0.69 px，无影响 |
| tethys | 900 | 8593.00,1408.62 | 8593,1382 | 差 26.6 px，1.7 格，见第五节 |
| **fabricatorium** | **905** | **2474,1957** | **7437,13783** | **Δ=(4963,11826)** |

**只有 fabricatorium 是灾难性的**；它是唯一一个「有分层包但没有任何校准条目」的 frame。

## 四、为什么三天都没人发现

它是**静默**的，而且症状指向错误的方向：

- 楼层识别、own-art 占比、去抖、near-tie 兜底全部正常工作，日志里 `identified=1 ownDominant=1`；
- 只有 `Contains` 永远 false，而 `Contains` 为 false 的表现就是「分层地图不生效」——
  看起来像"识别不出来"，实际是"识别出来了但不采纳"；
- 错误的数字不违法任何断言：`(2474,1957)` 和 `(7437,13783)` 都是"合理"的坐标原点，
  没有哪一层会因为它们而报错或越界；
- 索引里同一个 `coordinateTransform` 还被用来算 `centerMapX/centerMapY`（冷启动检索范围的中心），
  所以冷启动的范围提示也是错的。

## 五、修复

### 5.1 唯一的坐标系解析处：`scripts/SceneCoordinateTransform.ps1`（新增）

`Get-SceneCoordinateTransform -SourceRoot <root> -Frame <state> [-Scene <name>]`：

1. 从 `IMao-Core/src/Coordinate/CoordinateStruct.h` 解析编译期场景表（与运行时同一个来源）；
2. 若 `scene-calibrations.json` 里有该场景且 **`passed = true` 且 `maxErrorPixels ≤ 8`**，用它覆盖
   —— 与 `Scene::LoadExternalConfig` 完全同一道门槛，免得索引落在运行时**不用**的坐标系里；
3. frame 不在表里 → 报错（提示先把它加进运行时表）；
4. `-Scene` 给定时必须与表里的一致 → 拦住区域注册表与运行时的漂移；
5. 该 frame 需要四锚点校验但仍是 `(0,0)` 占位、且没有已通过的校准时 → 报错。
   `(0,0)` 和 World 的原点一样"看着合理、实际全错"，与其建一个永远包含不了玩家的 footprint，
   不如直接拒绝。

### 5.2 `scripts/New-LayeredFloorIndex.ps1`

删掉写死的 World 默认值，改调解析器，并打印实际来源：

```
  scene transform: origin=(7437,13783) scale=1.205 source=runtime-scene-table
```

### 5.3 重新生成 fabricatorium 的 `layered-floors/`（见第六节）

### 5.4 `scripts/LayeredOwnArtMask.ps1`：坐标系一致性硬校验

`Get-OwnArtMask` 新增 `-FrameMismatchTolerance`（默认 0.2），在抽样后检查关键点是否落回
`-TileOverlays`（= 这一层的特征就是从那几块 tile 上提的）。实测分离度是**全有或全无**：

| | tile 落回率 |
|---|---|
| 全部 90 层（各自索引的变换） | **100.0%** |
| 只把 transform 数字改对、`.imf` 仍是旧坐标系的那份（见第六节） | **0.0%** |

没有这道校验时，坐标系不匹配的结果是"找不到 overlay → 整张 mask 全 0"，而运行时的
`ownShare = 0` 会被读成"没有一个是本层自绘" → 静默把 own-art 否决关掉。现在它直接抛错。
`scripts/Set-LayeredOwnArtMask.ps1` 捕获该错并**跳过那一层**（保留原有 mask，不写错的值）。

### 5.5 `scripts/Test-KuroMapFeaturePack.ps1`

每个包若带 `layered-floors/floor-index.json`，就要求它的 `frame` 等于包 manifest 的 Kuro state，
且 `originX/originY/scale` 等于解析器的结果（容差 0.001）。已发布包全部通过。

### 5.6 `scripts/Refresh-MapTestBinaries.ps1`

- 新增：把共享 Assets 里的 `<region>/layered-floors/**` 同步进测试树。
  运行根持有的是 Assets 的**副本**，而这份数据是在仓库里改的——不定这个同步，
  修复进了 Assets 而受测的树里还是坏的（这次就是这么撞上的）。
- 报告新增 `frame=` 与 `transform=ok/WRONG FRAME` 两列，wrong frame 会另出一条警告。

## 六、为什么是重新生成，而不是把那两个数字改对

只改 `coordinateTransform` 会留下一个**新的**不一致：`.imf` 里的关键点坐标是**用旧变换烘进去的**
（`KuroTilePointToAppMap` 里 `world * scale + origin`）。抽查全部 90 层，用各自索引的变换反算：

- 所有正常楼层：**100% 的关键点落回自己的 tile**（多重集意义下）；
- 只改数字的 fabricatorium：**0.0%**。

后果不是运行时报错，而是 `Set-LayeredOwnArtMask.ps1` 再跑一次会把那一层的 mask 全写成 0，
静默关掉 own-art 否决——正好是本分支在修的那类故障。所以用**修好的生成器重新生成**：

前置条件核对：4 张 composite 瓦片与出厂包 manifest 里记录的 sha256 **完全一致**，因此描述子是
在同一批像素上重算的。新旧对照：

| 项 | 出厂包（原） | 重新生成（现在） |
|---|---|---|
| `coordinateTransform` | (2474,1957) ✗ | **(7437,13783)** ✓ |
| `keypointCount` | 3269 | 3269 |
| 关键点位置（多重集） | — | 与出厂包逐点相差**恰好 Δ=(4963,11826)**（3263/3269 位完全一致，6 个是 3 对近重复点交换了顺序） |
| 描述子 | — | **多重集完全相同**（仅那 6 行位置不同，即 3 对近重复关键点顺序互换） |
| `ownMask` | — | **逐位相同** |
| `occupancy` / `shared` 网格 | — | 相同 |
| 关键点 tile 落回率 | 0.0% | **100.0%** |
| 头部第 52–115 字节 | — | 一个 64 字节摘要，随源 XML 变（其余头字节相同） |

之所以是 3 对近重复点互换：OpenCV 的 SURF 在并行检出等响应关键点时顺序不稳定。因为交换的是
**描述子多重集**，BFMatcher 的候选集合不变，投票结果不受影响；`ownMask` 逐位相同也印证了
两个关键点落在同一格。除此之外没有任何东西引用这些 `.imf` 的哈希（仓库里没有
`baseline-files.json`），所以不存在需要同步的完整性清单。

## 七、复现与验证

```powershell
# 1) 解析出的坐标系（应为 7437,13783 / source=runtime-scene-table）
pwsh -File scripts\New-LayeredFloorIndex.ps1 -RegionId fabricatorium -OutputRoot out\verify

# 2) 每个包的索引变换都必须等于解析器结果
foreach ($d in Get-ChildItem Assets\FeaturesDatas\KuroTilePacks -Directory) {
    pwsh -File scripts\Test-KuroMapFeaturePack.ps1 -PackRoot $d.FullName }

# 3) 测试树：90/90 层有精确长度的 mask，10/10 个索引 transform=ok
pwsh -File scripts\Refresh-MapTestBinaries.ps1 -DryRun

# 4) 负例：坐标系不匹配时必须抛错（而不是写出全 0 的 mask）
#    Get-OwnArtMask 用「旧 .imf + 新 transform」→
#    100.0% of 3269 keypoints landed outside the tiles they were built from
```

验证结果：

- `Test-KuroMapFeaturePack.ps1`：10 个带分层包的区全部通过（含 fabricatorium，`layeredFloors=1`）；
- `Refresh-MapTestBinaries.ps1 -DryRun`：`floors=90 with mask=90 exact length=90 transforms=10/10`；
- 用日志里那一帧的坐标复算 `Contains`：`game=(−212.43,−533.55) tile=(0,1) cell=(48,23) occupancy=1
  → Contains=True`（原先 `tile=(5,−10)`，该层没有那块 tile）；
- tile 落回率：fabricatorium 现为 100.0%（3269/3269）；
- `IMaoLayeredMapTests` 通过；
- `Set-LayeredOwnArtMask.ps1 -Report -Region fabricatorium`：`own=1490/3269 outside=0`，
  与出厂 mask 逐位一致（1534 − 44 个共用地表位 = 1490）。

## 八、影响面

- **只有 fabricatorium 需要修**。其余 9 个分层包的索引坐标系与权威值一致（5 个 frame 8 用编译期值，
  4 个用已通过的校准）。
- 没有分层数据的区域：`avinoleum`(903)、`blackshores`、`timeriftruins`(910) 根本没有
  `layered-floors/`，属于**覆盖缺口**，不是这个 bug。
- tethys 的索引与包 manifest 差 26.6 px（1.7 格）——两者都在 `passed` 门槛内、运行时用的是校准值，
  本次不动，但已经记录在第五节 5.1 的门槛里。

## 九、遗留

- `lahai` 的包在 `Test-KuroMapFeaturePack.ps1` 的"同坐标同字节重复条目"检查上失败
  （`-5,8`）。这与本次改动无关（该检查只看 `manifest.json`，本次没动它），但既然跑出来了就先记着。
- 本修复针对的是 footprint 包含判定。in-cave 的采纳仍要同时满足 own-art 否决，
  两者互不替代：坐标系修好后，隐海试验场的地表位置仍然会被 own-art 挡住（全游戏扫描里该区
  68.4% → 0.0%）。
