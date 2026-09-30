# 梦枢天罗接入（2026-09-30，分支 `feature/mengshutianluo`）

上游随游戏版本更新推出了一张新地图 **梦枢天罗**。本文记录这次接入做了什么、依据是什么、还差什么。
所有数字都是本次实测（命令与输出见各节）。

## 一、它是什么（上游事实）

`GET https://api.kurobbs.com/map/core/position/getMapStateSelection` 现在返回 **9 个顶层 state**：

```
903 阿维纽林 | 902 下层金库 | 909 黯原 | 8 瑝珑、黑海岸群岛、黎那汐塔、罗伊冰原
900 泰缇斯之底 | 905 隐海试验场 | 910 时隙废都 | 912 梦枢天罗 | 906 罗伊冰原
```

资源代次从 `E62CEAC5…` 变成 **`13CCF182D6AF491CA1AC2A02754E5345`**。

| 项 | 值 | 来源 |
| --- | --- | --- |
| Kuro state | **912** | `getMapStateSelection` |
| 名称 | 梦枢天罗 | `country.json` 里瑝珑（`countryId=1`）的 level-2 条目 |
| 归属 | 瑝珑，`mapState` = **8**（和梦州相同） | 同上 |
| 子区域 | 7 个：烬心域 / 怡心域 / 凄心域 / 墟心域 / 沉心域 / 心相迷宫 / 相心域（`haveLayer` 全为 true） | `country.json` |
| 点位 | **20 类 / 231 点**（`stateId` 全为 912，`countryId` 全为 1） | `states/state-912.json` |
| 分层 | `layer.json` 有 6 组 8 层：朔寒窟 / 临渊窟 / 悬瀑秘窟 / 千绽窟 / 徊心墟 / 沉凄渡 | `mcmap/layer/<ver>/912/layer.json` |

**为什么它必须是一个独立地区**：地区身份是 `(frame, mapState)`。它的 `mapState` 是 8，而这个 8 在
瑝珑下已经被梦州占用——只靠 `mapState` 会把它并进梦州。它的 `frame` 是 912，与梦州的大世界 frame 8
不在同一个坐标平面上，所以 `region ⊆ frame` 的不变量自动把它拆成独立小世界。**这也是本次适配要特别
注意的一点**：它不是"梦州的新区域"，而是一个和大世界并列的独立小世界，自己一个包、自己一套原点。

## 二、做了哪些改动

### 1. 上游快照（`scripts/Sync-KuroMapData.ps1`）

- 新 state `'912' → MengshuTianluo`，`supported = $false`。
- 顺带修正一处会破坏门禁的漂移：`902/909/910` 此前被标成 `supported = $true`（2026-09-02 的提交
  `6d75fc9`），但它们的运行时点位一直是由 `Publish-KuroMapNewStates.ps1` 单独发布的，而
  `Test-KuroMapData.ps1` 的路线表里它们是空的。**`supported = $true` 会让它们的物品 id 进入
  `filter-items.json`，而那张表是不按场景开放状态过滤的**（`StringItems.cs` 只过滤
  `new-state-filter-items.json`）⟹ 一个还没开放的新地区会在场景获批之前就出现在筛选页里。
  现在四个"外部运行时点位"的 state 统一是 `supported = $false`，行为与仓库里已发布的快照一致。
- 同步结果（`-Apply`）：World **19184 → 19187**（+3）、黯原 **673 → 702 点 / 43 → 44 类**、
  下层金库有一条描述被上游改写、新增 state 912 与 10 张新图标；其余 state 未变。
- **图标文件名改成按 id 稳定分配**（见 §四）。

### 2. 运行时场景（原生 + 托管）

`IMao-Core/src/Coordinate/CoordinateStruct.h` 新增第 9 个场景：

```cpp
{ 9, "MengshuTianluo", 912, 0.0, 0.0, 1.205, true  }   // requiresGameValidation = true：原点还是占位值
```

同步改到的位置（凡是列举 8 个场景的地方都要加第 9 个）：

| 位置 | 改动 |
| --- | --- |
| `CoordinateStruct.h` | `definitions` / `sceneIds` / `sceneNames` |
| `DrawItemBase.h` / `.cpp` | 新增 `itemsJsonData_MengshuTianluo` + `case 9` |
| `DrawMarkerInteraction.cpp`、`ImGuiOverWindows.cpp` | 显示名表 / 字体字形表 |
| `RoutePlanningService.cpp` | 目录来源数组（**`std::array` 的大小 8 → 9**，下标 `i+1` 就是场景 id） |
| `MarkerCompletionStore.h` | `SceneState("MengshuTianluo") = 912`（点位身份 `<stateId>:<pointId>` 依赖它） |
| `MarkerDetailService.cs` / `LegacyPointRecovery.cs` | state↔scene 两个方向 |
| 测试 3 处 | `OptimizationTests.TestNewSceneRegistry` 现在断言 4 个新场景；两个测试宿主补上静态成员 |
| 脚本 | `Sync-KuroMapFeaturePack` / `Test-KuroMapFeaturePack` / `Set-KuroSceneCalibration` / `Start-KuroCaptureAssistant` / `Approve-KuroSceneRelease` / `Stage-UpdateResources` / `Test-ResourceUpdateStaging` / `New-MapRegionRegistry` / `Test-MapRegionRegistry` / `Measure-LayeredMapFeatures` / `New-VisualLocalizationManifest` / `tools/audit_game_data.py` |

场景**保持关闭**：`Assets/KuroMap/scene-validation.json` 里 `MengshuTianluo.approved = false`，
`kuro-tile-packs.json` **不登记**它（登记了却加载失败会让整个资源加载中止）。

### 3. 点位与筛选表（`Publish-KuroMapNewStates.ps1`）

四个 staged state 统一走这条路：生成 `runtime/itemsData_MengshuTianluo.json`（20 类 / 231 点，保留
`floorId`/`level`）并把它加进 `new-state-filter-items.json` + `new-state-item-scenes.json`。
`Test-KuroMapNewStates.ps1` 现在区分**已发布**与**已开放**：数量、筛选表、场景映射对四个场景都要成立，
瓦片包登记与 `approved=true` 只对"已开放"的三个要求，并**反过来拒绝**"未开放却已登记"的状态。

### 4. 瓦片窗口：新增"实测足迹"（`scripts/Get-MapTileFootprint.ps1` + 注册表 `footprint-measured`）

新 frame 的原点是编译期占位值，按点云分位数推窗口只能算"猜测"。但**瓦片窗口其实不需要原点**：
上游把影像放在哪几个格子上是能直接测的。

上游对"这张图上没有内容"的格子返回 **200 + 全透明占位图**（不是 404），所以只有字节数能区分。
实测 912：

```
搜索框 x -8..9, y -8..9（324 格，HEAD）
  服务器给了 60 格：有图 12 格（>= 100000 字节），占位 48 格
  尺寸直方图：22616×43  33897×4  56458×1 | 225665×2  315909×3  439995×3  1624447×4
  足迹：x -1..2, y -1..2（16 格，其中有图 12 格）
```

阈值 100 000 落在 56 KB 与 225 KB 之间，是 4 倍以上的空档。脚本还会在"有图格子碰到搜索框边缘"
（说明地图可能更大）和"有格子离阈值 ±25% 以内"（说明这一格是掷硬币）时警告。

注册表新增置信度 **`footprint-measured`**：窗口取实测包围盒（**不含覆盖边距**——测量就是地图本身），
该地区因此 `buildable = true`；同时因为原点是未证实的，报告把它单列一节"窗口已实测、但仍缺实机证据"。
生成器会拒绝：`state`/`scene` 与地区不符、代次与注册表代次不符（只警告并退回不可信窗口）、
**有点位落在实测包围盒之外**（那说明生效原点与实测影像互相矛盾）。

### 5. 瓦片归档与地图包

新 frame 的瓦片只存在于**当前代次**，所以**不能**用 `Get-MapTileArchive.ps1`：它走代次替换取证，
会和既有包 manifest 里记录的哈希逐一比对，而 912 在任何一个既有包里都不存在 ⟹ `compared == 0`
⟹ 直接抛 `refusing to archive unverified tiles`；它还会用只含所选地区的清单覆盖 `tiles.manifest.json`。
改用：

```powershell
pwsh -File scripts\Get-MapTileFootprint.ps1 -RegionId mengshutianluo -Download
# → map-regions/tiles/13CCF182D6AF491CA1AC2A02754E5345/912/912_<x>_<y>.png（12 块，9 216 830 字节）
# → map-regions/footprints/mengshutianluo.json（探测记录 + 包围盒 + 12 块 sha256）

pwsh -File scripts\Invoke-MapRegionRebuild.ps1 -Apply -RegionId mengshutianluo `
    -TileArchive C:\Dcode\WWMAP-TOOLS\map-regions\tiles\13CCF182D6AF491CA1AC2A02754E5345
```

构建结果（`out/map-regions/packs/mengshutianluo`，**不进 git**）：

```
tiles=12  keypoints=26 101  sceneId=9  anchor=(-425,-425)  bounds=x -1..2, y -1..2
features.imf 14 094 656 B / visual-index.imx 4 383 476 B / features.yml 50 948 444 B
referenceVerification = { skipped: true, passed: false, reason: 'No game minimap reference was supplied…' }
```

`Test-KuroMapFeaturePack.ps1 -AllowUnverified` 通过。**这是未验证覆盖包**：没有参考小地图，
`KuroTileFeaturePack.cpp` 的运行期门禁不会放行，所以它现在只能离线用。

## 三、四点校准（2026-09-30 已完成）

用户当晚提供了 **4 对截图**（`C:\Users\Kahvia\Videos\NVIDIA\Wuthering Waves`，19:03–19:06）：
每对是"正常画面（左下角有游戏坐标）+ 紧接着的大地图"。正常画面读出的游戏平面坐标：

| # | 正常画面 | 大地图 | 游戏坐标 |
| --- | --- | --- | --- |
| 1 | 19:03:42.39 | 19:03:50.34 | **(-413, -209)** |
| 2 | 19:04:14.83 | 19:04:18.25 | **(-179, 281)** |
| 3 | 19:05:09.86 | 19:05:14.41 | **(409, 390)** |
| 4 | 19:06:03.74 | 19:06:06.50 | **(490, -124)** |

`map`（内部地图像素坐标）由**大地图视口定位**离线量出——这是首次把这条方法完整写进文档，
命令与几何见 [新地区四点校准样本](KuroSceneCalibrationSamples.md#map-值怎么量2026-09-30-实测确定以前没写下来)。
四张大地图全部解出 `sceneId = 9`、matches 126~368、inliers 122~364。

```
scale  = 1.20700208
origin = (-0.127, -0.511)
maxError = 0.534 px            （门限 8 px；既有五个场景是 0.20 ~ 0.82 px）
```

⟹ **编译期占位原点 `(0, 0)` 是对的**，这个 frame 和其他三个独立小世界（下层金库 −3.5/−2.5、
黯原 −0.4/0.2、时隙废都 −1.8/−1.9）一个规律。样本存 `map-regions/samples/mengshutianluo.json`。

> **为什么用视口中心而不是箭头质心**：两件事都测了。用中心拟合 max **0.534 px**，用箭头像素质心拟合
> max **2.247 px**。箭头质心在 y 上系统性偏下约 2.4 屏幕像素（图标字形的锚点与着色像素质心不重合，
> 且第 1 张的箭头还压在传送环上）。两条都过门限，但中心那条好一倍——这也反过来证明
> **游戏打开大地图时确实把视口居中在玩家身上**。

`Assets/KuroMap/scene-calibrations.json` 已写入该记录，注册表里梦枢天罗随之变成 `calibrated`
（窗口仍然来自实测足迹，`tileBounds.basis` 记着这件事）。**这不会让地区对玩家开放**：
`scene-validation.json` 里它仍是 `approved = false`。

## 四、还差什么

1. **一张参考小地图** —— `map-regions/references/mengshutianluo.png`，包就会从"未验证覆盖包"变成已验证包
   （门限 8 px）。⚠️ 它必须拍在**注册表给出的锚点**上：梦枢天罗是游戏坐标 **(-425, -425)**（窗口中心），
   因为参考验证是拿"该点的期望地图像素"去比对定位结果。**用户已有的四张截图都在别的位置，不能直接用。**
   （另一条路是把锚点改成其中一张截图的位置——那需要给 `Invoke-MapRegionRebuild` 开一个锚点覆盖参数。）
2. 之后才是登记 `kuro-tile-packs.json` → `Approve-KuroSceneRelease.ps1 -Region mengshutianluo`
   （还要求实机四项检查与 113 张基线回归）。
3. **分层地图未归档**：6 组 8 层（朔寒窟/临渊窟/悬瀑秘窟/千绽窟/徊心墟/沉凄渡）。
   `Get-MapLayerArchive.ps1` / `New-LayeredFloorIndex.ps1` 都按 `tiles.manifest.json` 的旧代次取图层。

## 五、⚠️ 发布这条资源时必须带 `minAppVersion`

`ResourceSnapshotContext.cpp` 的 `CheckConfig`（第 80 行）对 `scene-validation.json` / `scene-calibrations.json`
里的**每一个**场景名做 `Definition(scene) != nullptr` 检查，**不认识就整份快照验证失败**：

```
unknown scene in scene-validation
```

本次给 `scene-validation.json` 加了 `MengshuTianluo`。于是：

- **旧程序 + 新 map-data ⟹ 资源加载直接失败**（表现是"启动核心失败"）。
  下载侧有保护：`UpdateService.cs:621-622` 按 `MinAppVersion ≤ 本程序版本 ≤ MaxAppVersion` 过滤资源发布。
  **所以发布这份资源时，`minAppVersion` 必须设成"第一个带场景 9 的程序版本"**，否则老客户端会拿到它并崩在验证上。
- 反过来（新程序 + 旧 map-data）由 `mappedScenes.size() == Scene::definitions.size()`（第 315 行）
  拦下，也是硬失败；正常发布里 map-data 随程序自带（bundled），两者天然同步。

这条对**每一次加新地图**都成立，不只是这次。`out/map-test` 这次就是被它命中的：
测试树的 `Assets/KuroMap` 是指向 `x64/Release/Assets/KuroMap` 的联接（数据跟着源码走），
而树里的 `IMao-CoreHost.exe` 还是 09-28 的旧二进制 ⟹ 全量门禁在"原生区域选择检查"处报同一条错误。
**加了场景定义之后必须 `Refresh-MapTestBinaries.ps1` 刷新测试树**（它要求 `x64/Release` 是自包含构建，
framework-dependent 的 `dotnet build` 会被它拒绝——这正是它注释里写的那件事）。

## 六、顺手修掉的两个缺陷
1. **图标文件名不稳定**（`Sync-KuroMapData.ps1`）：原来按"排序后 id 列表里的位次"编号，
   插入一个新 state 会让位次整体平移 ⟹ 本次同步改写了 **527 张里的 469 张**，而真正新增的只有 10 张。
   现在：**老 id 保留原名，只有新 id 取一个没人占用的编号**（先给所有存活 id 占位，再分配新编号——
   边扫边占会把新 id 的编号抢到后面 id 的头上，那样仍然会整体平移）。改完同一次同步只动
   **10 张新增 + 2 张上游真的改过**。id 消失时它的文件在下一次 `-Apply` 被删掉（清单是这些名字的唯一引用）。
2. **未验证包的 report 说反了**（`Sync-KuroMapFeaturePack.ps1`）：`manifest.json` 写
   `skipped: true`，`report.md` 却打 `skipped=False`。原因是在 `[ordered]` 字典上查
   `PSObject.Properties['skipped']` —— **字典的键不在 `PSObject.Properties` 里**，于是永远走 `else { $false }`。

## 七、本次验证（全部实跑）

| 检查 | 结果 |
| --- | --- |
| `Build-IMao.ps1 -Parallel 8` | 0 error；`--check-resources` 打印 `resourcesReady: true`（9 个场景全部映射） |
| `ctest` | **7/7 通过** |
| `Test-KuroMapData.ps1` | 通过（13CCF…、537 图标、506 筛选 id） |
| `Test-KuroMapNewStates.ps1` | 通过（4 场景 / 110 类 / 1230 点 / 91 个物品 id） |
| `Test-MapRegionRegistry.ps1` | 通过（14 地区、24066 点、跨国冲突 477） |
| `Test-KuroMapFeaturePack.ps1 -AllowUnverified` | 通过（未验证覆盖包） |
| `Refresh-MapTestBinaries.ps1` | 刷新 27 个文件（场景定义变了就必须刷，见 §五） |
| `IMaoVisualRegression` 视口定位四张大地图 | 4/4 `accepted`、`sceneId=9`，matches 126~368 |
| `Set-KuroSceneCalibration.ps1 -Check` | scale 1.20700208、origin (-0.127,-0.511)、**maxError 0.534 px** |
| `Test-Runtime.ps1` 全量门禁 | **通过，exit 0**（`Runtime tests passed. Evidence: out\system-audit`） |

> 门禁第一次跑**失败**在"原生区域选择检查"：`unknown scene in scene-validation`。
> 原因就是 §五 那条——测试树的数据跟着源码走、二进制没跟着刷。刷新后重跑通过。
> 另有一次 `route-service-tests` 报 `Unable to commit user data: Windows error 5`
> （`AtomicFile.h` 的 `MoveFileExW` 瞬时被拒），**同一二进制、同一目录立刻重跑即通过**，
> 属 Windows 上的偶发占用，与本次改动无关。

## 八、给以后接新地图的人

顺序是：**同步点位 → 加一行场景定义并 `requiresGameValidation = true` → 生成注册表（拿到
`untrustedPointWindow`）→ 实测瓦片足迹并 `-Download` → 再生成注册表（变 `footprint-measured`）
→ 用实测代次目录重建包 → 采四组截图做四点校准 → 在锚点上补一张参考小地图 → 才谈登记与开放**。
`map-regions/README.md` §八 有可复制的命令清单，
`Docs/KuroSceneCalibrationSamples.md` 有 `map` 值的离线量法。
