# 地图地区重构（refactor/map-regions）

本目录是地图资源**全量重做**的输入与证据。仓库里只提交输入，**重新生成的特征产物不提交**（见"不进 git 的东西"）。

## 一、分割模型

一个地区由 `(frame, mapState)` 唯一确定：

- **`frame`** = 库街区顶层 `state`。它是**坐标平面**：只有同一 frame 内的坐标可比。大世界是 frame 8，每个独立小世界各占一个 frame。
- **`mapState`** = 上游地图前端的地区 id。地区名取自顶层 country 的 `mapStateName`。

两个都必须用，因为 `mapState=3` 同时被"拉古那（大世界）"和三个独立小世界（下层金库/阿维纽林/隐海试验场）复用，只有 frame 能把它们分开。

**不变量：`region ⊆ frame`，一个地区永不跨两个 frame。** 这一条让"独立小世界先拆出来"自动成立。

```
frame 8（大世界平面）
├── mapState 1  今州        jinzhou       11 个子区域
├── mapState 8  梦州        mengzhou       4
├── mapState 3  拉古那      laguna        10
├── mapState 4  七丘        qiqiu          2
├── mapState 6  冰原地表    roysurface     5
└── (空)        黑海岸群岛  blackshores    1
独立小世界（各自一个 frame）
├── 906 拉海洛   lahai      9 个子区域
├── 909 黯原     darkplain  4
├── 902 下层金库 lowervault 1
├── 903 阿维纽林 avinoleum  1
├── 905 隐海试验场 fabricatorium 1
├── 900 泰缇斯之底 tethys   1
├── 910 时隙废都 timeriftruins 1
└── 912 梦枢天罗 mengshutianluo 1
```

**14 个地区、52 个子区域、24066 个点位（大世界 19187）全部归入，0 冲突。**

> 梦枢天罗挂在瑝珑（`countryId=1`）下，而且它的 `mapState` 也是 **8**（与梦州相同）。它仍然是**独立地区**：
> 地区身份是 `(frame, mapState)`，frame 912 把它和梦州分开，`region ⊆ frame` 的不变量照旧成立。

## 二、归因规则

```
1. frame = 点位自身的 stateId                        （权威）
2. frame != 8  ->  该 frame 就是该地区
3. frame == 8  ->  只在【同一 countryId】的子区域锚点里找最近锚点
                   region = 该锚点的 mapState
```

**第 3 步的 `countryId` 过滤是硬要求。** 不加过滤时有 **477** 个点被分到别的国家，而且**全部是跨国冲突、没有一例同国家内部冲突**——说明不同 country 的原始坐标不在同一个画布上，跨国算距离本身无意义。`scripts/Test-MapRegionRegistry.ps1` 会把 477 作为证据打印出来。

## 三、坐标换算（已用地面真值验证）

```
game  = (raw - origin) / 100
tileX = floor(gameX / 850 + 1)
tileY = ceil(-gameY / 850)
```

`origin` 取 `IMao-Core/src/Coordinate/CoordinateStruct.h` 的编译期值，被 `Assets/KuroMap/scene-calibrations.json` 中 `passed=true` 的条目覆盖。

验证依据：**14 个独立小世界子区域锚点全部落在其既有包的瓦片矩形内**（Lahai 9/9、Darkplain 4/4、Tethys 1/1）；黑海岸群岛的标签 raw `(306821, 128753)` 换算为 `(3043, 1268)`，而 `BlackShores` 包的锚点是 `(2918, 1246.6)`。

> 注意：1.205 的 `scale` 是**地图影像显示比例**，不参与瓦片网格。早期误用 `raw/100*scale + origin` 会算错（会让 BlackShores 的标签落到 tile 8，而包只覆盖到 7）。

## 四、瓦片窗口

窗口由该地区点位的 **p1..p99 分位数**推导，再**每侧外扩 2 块**（`-CoverageMargin`）。

外扩是必需的：分位数只保证覆盖收集品所在处，而最小地图匹配必须在玩家能站到的任何位置工作。不外扩时黑海岸群岛只有 2 块，明显不足。

交叉验证：**梦州推导出 63 块，而旧的 Dreamzhou 49 + DreamzhouWest 9 = 58 块**，两者高度吻合。

| 地区 | 窗口 | 瓦片 | 置信度 |
| --- | --- | --- | --- |
| jinzhou 今州 | x -3..14, y -10..11 | 396 | validated |
| laguna 拉古那 | x 6..15, y -12..-1 | 120 | validated |
| roysurface 冰原地表 | x -4..4, y 4..13 | 90 | validated |
| qiqiu 七丘 | x 11..18, y -18..-10 | 72 | validated |
| mengzhou 梦州 | x -12..-4, y -2..4 | 63 | validated |
| blackshores 黑海岸群岛 | x 2..6, y -3..2 | 30 | validated |
| lahai 拉海洛 | x -7..4, y 3..13 | 132 | calibrated |
| darkplain 黯原 | x -2..5, y -3..3 | 56 | calibrated |
| tethys 泰缇斯之底 | x -2..3, y -3..2 | 36 | calibrated |
| avinoleum 阿维纽林 | x -1..14, y -13..2 | 400 | **uncalibrated** |
| fabricatorium 隐海试验场 | x -3..5, y -2..2 | 117 | **uncalibrated** |
| lowervault 下层金库 | x 2..4, y -1..1 | 9 | **origin-verified**（已收紧） |
| timeriftruins 时隙废都 | — | — | **blocked** |
| mengshutianluo 梦枢天罗 | x -1..2, y -1..2 | 12（窗口 16） | **footprint-measured** |

置信度含义：

- `validated`：frame 8，换算用 6 个既有包做过地面真值验证。
- `calibrated`：该 frame 有通过的四点校准。
- `origin-verified`：编译期原点由实机截图证实。**瓦片网格只需要原点**，所以这足以推导可信窗口，但它不是四点校准，也不能开放该地区。
- `footprint-measured`：窗口取**实测**的上游有图瓦片包围盒（`map-regions/footprints/<region>.json`），见下一节。
  **窗口与原点无关**，所以在一个 frame 的原点还没被实机证实之前，它就是那个 frame 唯一可信的窗口。
  它代替的是"按点位分位数猜窗口"，因此**不含覆盖边距**：测量结果就是地图本身，不是地图的下界。
- `uncalibrated`：无校准也无原点证据，窗口只是猜测，**不得据此发布**。
- `blocked`：frame 原点仍是编译期占位值 `(0, 0)`，且无证据。

**只剩 `时隙废都`(910) 仍被阻塞。** `下层金库`(902) 已由 `map-regions/origins/lowervault.json` 的实机证据解除阻塞。

### 实测瓦片足迹（`-SearchMargin` / `map-regions/footprints/`）

上游对**只有部分格子在服务**的地图会返回 **200 + 全透明占位图**，所以"服务器给了这个文件"和"这张格子上有图"
是两个不同的问题。只有字节数能把它们分开：实测该 frame 的占位图是 **22 616 / 33 897 / 56 458** 字节，
而最小的真实地图像素块是 **225 665** 字节——10 倍量级差。

```powershell
pwsh -File scripts\Get-MapTileFootprint.ps1 -RegionId mengshutianluo
pwsh -File scripts\Get-MapTileFootprint.ps1 -RegionId mengshutianluo -Download
```

脚本用 **HEAD + Content-Length**（不下载正文）在"点位窗口 ± `-SearchMargin` 块"的方框里逐格探测，
输出尺寸直方图，然后：

- 把 ≥ `-ImageryMinBytes`（默认 100 000）的格子判为有图，取它们的**包围盒**作为该 frame 的窗口；
- 探测范围**没有 404 只有 200/404 之外的响应**才继续，否则判定代次或 URL 形状不对并中止；
- 有格子落在搜索框边缘时**警告**——那说明地图可能比测到的更大，要加大 `-SearchMargin`；
- 有格子的字节数在阈值 ±25% 内时**警告**——那说明这一格是"掷硬币"，要人工看直方图再定阈值。

`-Download` 把有图的格子取进 `map-regions/tiles/<generation>/<state>/`，并在证据文件里记下每块的 sha256。

**为什么必须新增这个脚本、不能直接用 `Get-MapTileArchive.ps1`**：新 frame 的瓦片只存在于
**当前代次**，而归档代次是旧的（`B50F…`）。`Get-MapTileArchive.ps1` 走的是"代次替换"路径：它会拿新代次的瓦片
和**既有包 manifest 里记录的哈希**逐一比对，而新 frame 在任何一个既有包里都不存在 ⟹ `compared == 0`
⟹ 直接抛错 `refusing to archive unverified tiles`。它还会用"只含所选地区"的清单**覆盖**
`tiles.manifest.json`，把另外 13 个地区的归档记录改掉。所以新 frame 的瓦片走独立目录 + 独立证据文件：

```
map-regions/tiles/13CCF182D6AF491CA1AC2A02754E5345/912/912_<x>_<y>.png
map-regions/footprints/mengshutianluo.json         ← 探测记录 + 包围盒 + 每块 sha256
```

生成器会校验证据文件：`state` 必须等于该地区的 frame、`scene` 必须一致、**代次必须与注册表代次相同**
（不同则警告并退回不可信窗口，不会静默用一个过期测量），并且**该地区的每个点位都必须落在实测包围盒内**
——否则说明"生效原点"和"实测影像"互相矛盾，直接报错而不是继续。

### 原点证据（`map-regions/origins/<region>.json`）

编译期原点为 `(0, 0)` 的 frame 原本无法推导窗口。证据文件的原理：`game = (raw - origin)/100`，**原点错误会让整个点云均匀平移**，所以正确的原点会把实机采集到的坐标放在真实收集点之上。

下层金库的实测：区域内点云横跨约 7210 × 3524 游戏单位，四张实机截图读出的坐标到最近收集点的距离分别是 **2.0 / 5.5 / 24.5 / 8.1 单位**。这不可能是巧合，因此 `(0, 0)` 成立。

生成器会校验证据文件：至少 4 个样本、每个样本到最近收集点的距离不超过 `toleranceUnits`、且 `origin` 必须与生效原点一致，否则报错。

**证据文件目前只填了 `game` 坐标，`map` 留空。** 要生成正式的 `scene-calibrations.json` 记录，还差把每张大地图截图里玩家箭头的位置换算成内部地图像素（方法见下）。

### 窗口收紧（`-TightenRegionId`）

点位推导的矩形窗口会被**少量离群点严重撑大**。下层金库请求 99 块，但上游真实只提供紧凑的 3×3 = 9 块，原因是 6 个离群点挤在遥远的 tile(9,-3)。

`-TightenRegionId <id>` 把该地区的窗口**与归档里实际存在的瓦片足迹求交集**（只收紧、绝不扩张）。因为矩形窗口表达不了非矩形足迹，收紧结果是现存瓦片的**包围盒**，非矩形地区仍会包含少量不存在的瓦片（构建时计为 missing，无害）。

收紧只在两种情况下允许：归档代次与注册表代次相同，或者归档是**经过逐字节取证**的代次替换（`verification.performed && mismatched == 0`）。否则直接报错——否则一个陈旧或不完整的归档会静默缩小覆盖范围。

```powershell
pwsh -File scripts\New-MapRegionRegistry.ps1 -TightenRegionId lowervault
```

**未收紧的地区仍保留推导窗口**：`jinzhou 196/396`、`tethys 13/36`、`darkplain 25/56` 等都有虚高，逐一复核后再决定是否收紧。归档清单里每个地区的现存/请求数可以直接用来判断。

## 五、上游代次已变更（重要）

归档数据的 `resourceVersion` 是 `E62CEAC5F80745288BF76C7AD5F731C3`，但**该代次已经 404**。当前上游是 `B50F4135DCCC4D8DA87ED33CE95EA31D`。

已取证：用当前代次下载的瓦片，与旧包 manifest 里记录的 sha256 **逐一比对 302 块，0 个不一致**，且新代 `country.json` 的 51 个子区域与归档版**完全一致（0 新增 0 移除）**。结论：**上游只是换了版本目录名（缓存刷新），地图影像和地区结构没有变**，锚点与校准依然有效。

这正是"靠版本号做复现"不可靠的实证——所以瓦片被**归档到本地**，重建不再依赖网络。

## 六、脚本

| 脚本 | 作用 |
| --- | --- |
| `scripts/New-MapRegionRegistry.ps1` | 从 `Assets/KuroMap` 生成 `regions.json` + `regions.report.md` |
| `scripts/Test-MapRegionRegistry.ps1` | 独立重新推导归因并校验注册表（结构/覆盖/frame/窗口/点归属） |
| `scripts/Get-MapTileArchive.ps1` | 按注册表下载瓦片到 `map-regions/tiles/<generation>/`，跨地区自动去重，记录 sha256 |
| `scripts/Get-MapTileFootprint.ps1` | **测量**某个 frame 实际上有图的格子（HEAD + Content-Length），写 `map-regions/footprints/<region>.json`；`-Download` 顺带取回瓦片 |
| `scripts/Invoke-MapRegionRebuild.ps1` | 按注册表逐个地区重建特征包，产物写到 `out/map-regions/packs/` |

重建脚本的补强：`Sync-KuroMapFeaturePack.ps1` 新增 `-TileArchive`（离线、可复现）、`-ResourceVersion`（固定代次，漂移即失败）、`-MaxTiles`（原为硬编码 256）、`-OutputRoot`（产物重定向出仓库）。

## 七、可实机测试的构建

运行时有两道硬门禁（`KuroTileFeaturePack.cpp`）：`referenceVerification.passed` 必须为真（第 188 行，否则整包加载失败），且场景必须被批准（`CoordinateStruct.h:140` 的 `scene-validation.json`）。所以"未验证覆盖包"只能离线用，进不了游戏。

**复用旧包的锚点与参考小地图**是让它通过的最短路径：`-UseShippedReference` 会取 `Assets/FeaturesDatas/KuroTilePacks/<同名目录>/` 的 `anchorWorldCoordinate` 和 `reference-minimap.png`。这两者是**观测值**（维护者在游戏里读出的坐标 + 在该处截的小地图），任何管线都推导不出来；而参考验证是可证伪的——配对错了会直接失败，不会静默产出坏包。

实测（第 4 轮）：roysurface `errorPixels=3.18`、darkplain `errorPixels=3.07`（门限 8），且 darkplain 的 `expectedMapCoordinate` 与旧包记录**完全一致**。

**测试树**：不要往 `x64/Release` 或源码 `Assets` 里装包——`x64/Release/Assets` 是**真实目录**（含 `bundled-snapshot.json` 等构建产物），覆盖它会污染源码树和 LFS。用：

```powershell
pwsh -File scripts\New-MapTestTree.ps1 -PackRegionId darkplain,roysurface
```

它在 `out/map-test/` 组装一个独立测试根：二进制从 `x64/Release` 复制、未替换的 Assets 条目用链接指向已构建的 Assets、被替换的两个包是真实目录、并重写注册表只列出**实际存在**的目录（注册了却没有目录的包在快照路径下会让整个资源加载抛错——源码注册表里的 `LowerVault`/`TimeRiftRuins` 就是这种情况）。

产物：`out/map-test/IMao-WinUI.exe`（**以管理员身份运行**）。源码树与 `x64/Release` 均不被改动。

## 八、执行顺序

```powershell
# 0. 生成/校验注册表
pwsh -File scripts\New-MapRegionRegistry.ps1
pwsh -File scripts\Test-MapRegionRegistry.ps1

# 1. 归档瓦片（当前代次；替换代次时会强制逐字节取证）
pwsh -File scripts\Get-MapTileArchive.ps1 -ResolveCurrentVersion -ThrottleLimit 8

# 2. 校验重建计划（不构建）
pwsh -File scripts\Invoke-MapRegionRebuild.ps1

# 3. 构建（单个地区先试点）
. .\scripts\Enter-DevEnvironment.ps1
pwsh -File scripts\Invoke-MapRegionRebuild.ps1 -Apply -RegionId blackshores

# 4. 全部可构建地区
pwsh -File scripts\Invoke-MapRegionRebuild.ps1 -Apply
```

### 接入一个全新 frame（新地图）的顺序

新地图的 frame 在原点上没有校准、在归档里也没有记录，所以前两步与上面不同：

```powershell
# 1. 同步上游点位（新 state 先按 supported=$false 归档，见 Docs/KuroMapDataSource.md）
pwsh -File scripts\Sync-KuroMapData.ps1 -Check
pwsh -File scripts\Sync-KuroMapData.ps1 -Apply

# 2. 在 IMao-Core/src/Coordinate/CoordinateStruct.h 里加一行运行期场景定义
#    （compile 期原点先留 (0,0)，requiresGameValidation = true），再生成注册表：
#    此时该地区是 blocked，但会带上一条 untrustedPointWindow 供下一步限定搜索范围
pwsh -File scripts\New-MapRegionRegistry.ps1

# 3. 实测该 frame 的瓦片足迹并取回瓦片（不依赖原点）
pwsh -File scripts\Get-MapTileFootprint.ps1 -RegionId <id> -Download

# 4. 再生成注册表：该地区变成 footprint-measured、buildable
pwsh -File scripts\New-MapRegionRegistry.ps1
pwsh -File scripts\Test-MapRegionRegistry.ps1

# 5. 用**实测代次目录**重建地图包（不能用 tiles.manifest.json 里那个旧代次）
pwsh -File scripts\Invoke-MapRegionRebuild.ps1 -Apply -RegionId <id> `
    -TileArchive map-regions\tiles\13CCF182D6AF491CA1AC2A02754E5345
```

没有 `map-regions/references/<id>.png` 时第 5 步会构建**未验证覆盖包**（`referenceVerification.skipped = true`）。
它可以离线检查，但**进不了运行时**（`KuroTileFeaturePack.cpp` 要求 `passed == true`），所以
**不要**把这样的包登记进 `Assets/FeaturesDatas/kuro-tile-packs.json`：注册表里"登记了但加载失败"的包会让
整个资源加载中止（表现是"启动核心失败"）。`scripts/Test-KuroMapNewStates.ps1` 会拒绝"未开放却已登记"的状态。

## 九、当前状态与未决项

**已完成**：注册表（14 地区，校验全绿）；瓦片归档（既有 13 地区 638 块，跨地区去重；梦枢天罗另在 13CCF 代次下 12 块）；代次替换取证（302/302 一致）；五个脚本；重建脚本补强；下层金库解除阻塞并**收紧到 9 块、建成完整包**（9008 关键点，`Test-KuroMapFeaturePack.ps1` 通过）；blackshores 试点（28 块、15168 关键点）；**梦枢天罗接入**（点位/图标/筛选表已入库，实测足迹 12 块 / 窗口 16 块，地图包已建）。

**未决项**

1. **梦枢天罗缺实机证据**：四点校准（4 个位置各一张正常画面 + 一张大地图）与参考小地图。
   在此之前它保持 `approved=false`，不进筛选、标注与视觉定位；地图包是未验证覆盖包，不能登记进瓦片包注册表。
   采样位置命令见 [新地区四点校准样本](../Docs/KuroSceneCalibrationSamples.md)。
2. **梦枢天罗的分层地图**（`layer.json` 里 6 个窟 / 8 层）尚未归档：`Get-MapLayerArchive.ps1` 与
   `New-LayeredFloorIndex.ps1` 都按 `tiles.manifest.json` 的代次取图层，而该文件的代次是旧代次。
   要接分层，先把梦枢天罗的图层单独归档，再让这两个脚本接受显式代次。
3. **时隙废都(910) 缺原点证据或校准**，阿维纽林(903)、隐海试验场(905) 缺校准。按用户要求先放着，不要求一次做完。

**未决项**

1. **时隙废都(910) 缺原点证据或校准**，阿维纽林(903)、隐海试验场(905) 缺校准。按用户要求先放着，不要求一次做完。
2. **下层金库的四点校准记录还差一步**：`map-regions/origins/lowervault.json` 里 4 个 `game` 坐标已经读出（截图在 `C:\Users\Kahvia\Videos\NVIDIA\Wuthering Waves`，成对的时间戳见文件里的 `capture`/`mapCapture` 字段），`map` 留空待填。填法是"把大地图箭头中心换算为内部地图像素"：仓库里唯一的完整样例是 `Tests/VisualLocalization/20260909-sparse-minimap/provenance.json`（含 `mapCaptureCorners` 与 `arrowNominalScreen`），消费方是 `IMao-Core/tests/SparseMinimapTests.h`。**该四角是把大地图裁剪图独立匹配到地图影像上得到的**，所以先要有该地区的地图影像——这也是为什么先建包、后校准是自然的顺序。填好后跑 `scripts/Set-KuroSceneCalibration.ps1`。
2. **参考小地图**：`map-regions/references/<region>.png` 放一张实机小地图截图后，该地区就会构建成**已验证**包；没有的话构建会明确标记为 unverified 覆盖包（`referenceVerification.skipped=true`）。现有 4 张可复用的参考：`Assets/FeaturesDatas/KuroTilePacks/*/reference-minimap.png`。
3. **`legacyBaseExclusions` 需要重新生成**。旧包 `Tethys`/`Lahai` 带有这个字段，它记录的是**与该包特征重复的基线 IMF 行号**（由 `IMao-Core/src/Feature/LegacyFeatureExclusions.h` 在运行时排除）。旧脚本在特征哈希变化时拒绝继承，而重建必然变化，所以：
   - `-OutputRoot` 指向全新目录时该门禁自动跳过（干净重做语义）；
   - 但如果新包的瓦片仍与旧基线图集重叠，就必须用 `scripts/Register-LegacySceneFeatures.py` **重新计算排除集**，否则运行时会双重匹配。**这是发布前必须验证的一项。**
4. **覆盖边距 `-CoverageMargin`（默认 2）需要实机确认**。窗口偏小时走路到区域边缘会匹配失败；偏大只是多下几块瓦片。
5. **上游 41%（326/795）的瓦片不存在**，因为矩形窗口覆盖了不规则地图之外的空白。这与旧包的情况一致（旧包缺失率 20–45%），但需要逐地区复核窗口形状。

## 十、不进 git 的东西

`.gitignore` 已忽略 `out/`。此外**不要提交**：

- `map-regions/tiles/`（上游瓦片归档，230 MB；可重建，且是第三方公开素材）
- `out/map-regions/packs/`（`features.imf` / `features.yml` / `visual-index.imx`）

提交：`regions.json`、`regions.report.md`、`tiles.manifest.json`（哈希清单）、`footprints/`（实测足迹与每块 sha256，
体积只有几十 KB，是**唯一**记录新 frame 瓦片身份的证据）、`references/`（自己的实机截图）、以及所有脚本。

理由：现有 LFS 已有约 1.74 GB（其中 `features.yml` 占 879 MB，而**运行时只做存在性检查、内容根本不读**）。把重生成的特征提交进 LFS 会让每代再增加约 1.2 GB，而 LFS 不会因新提交释放旧对象。
