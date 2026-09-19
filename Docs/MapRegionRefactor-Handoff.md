# 地图资源分区域重构 · 交接说明

这份文档写给**接手这项工作的人或 AI 会话**。目标不是复述历史，而是让你在
**不重新踩一遍坑**的前提下继续往下做。

如果你是一个新会话，请先执行下面的"自证状态"，不要只信本文的描述——本文也可能有记错的地方。

```powershell
cd C:\Dcode\WWMAP-TOOLS
git log --oneline -6          # 期望 HEAD = 222ace2
git rev-parse --abbrev-ref HEAD   # 期望 refactor/map-regions
git status --porcelain=v1     # 期望无输出
```

---

## 一、大目标

把一份**融合在一起**的地图资源，拆成**按区域独立**的包，从而支持：

1. 用户按区域选择性下载 / 删除
2. 增量更新（只下载变化的包）
3. 停止把地图资源打进程序包
4. 最终取消掉历史遗留的融合产物

项目本身是 WuWa 地图 overlay 工具：C++ 的 `IMao-CoreHost`（定位、渲染、IPC）
+ WinUI 宿主 + 一条资源更新通道（`IMao-WinUI.Core/Updates`）。

---

## 二、已完成（截至 `222ace2`）

| 提交 | 内容 |
|---|---|
| `dc5c9bb` | 13 个区域包落地源码，替换旧的融合 7 包（含 `Dreamzhou` / `DreamzhouWest`） |
| `ee76fd5` | 新增**可选** `MapIconRoot` |
| `f83ea23` | 图标拆成 `map-icons` 包 |
| `222ace2` | 新增**可选** `MapFeatureRoot`（150.3 MB 基础地图特征的分离管道） |

具体数字：

- 区域包：13 个，`Assets/FeaturesDatas/KuroTilePacks/<regionId>/`，合计约 536 MB。
  区域 = `(frame, mapState)`；`mapState=3` 被拉古那和三个小世界复用，**只有 `frame` 能分开**。
- 图标拆包：`map-data` 从 **560 文件 / 51.20 MB** 降到 **32 文件 / 16.35 MB**，
  `map-icons`（`KuroMapIcons/`）**528 文件 / 34.85 MB**。
- 程序包（`updates/stable.json` 记录的 2026.9.18.2）：**981.9 MB**，
  其中 `Map_features.imf` 127.4 MB + `Map_visual_index.imx` 22.9 MB = **150.3 MB（15.3%）**。

`MapIconRoot` 与 `MapFeatureRoot` 都是**可选且带回退**的：

```
快照里有该字段 → 用它
没有 / 为空     → 回退到 MapDataRoot() / BaselineRoot()/FeaturesDatas
```

所以这两步落地时都是**惰性**的：没有任何快照写这两个字段，现有布局行为逐字节不变。

---

## 三、本轮关键结论：基础库有多冗余（已实测）

用 `scripts/Register-LegacySceneFeatures.py` 对 13 个区域包逐一配准基础库
（`Map_features.imf`，**247,389** 个关键点）：

```
并集     = 218,615 行  =  88.4%
未覆盖   =  28,774 行  =  11.6%
```

**未覆盖点的空间形态才是关键判据**：

```
matched   坐标范围 = x 295..24635   y 216..19077
uncovered 坐标范围 = x 408..24629   y  92..18931
uncovered 落在 matched 包围盒【内部】 = 28,772 / 28,774  (100.0%)
落在外部                              =      2
```

→ 未覆盖部分是**散布的孔洞**，不是"漏掉了一整片地"。形态上更像
"同一地形在不同缩放/裁剪下检出的关键点不同"，而不像"我们没覆盖那片区域"。

**结论**：基础库基本冗余，但**不能证明删掉它零风险**。88.4% 是硬证据，
11.6% 只是"未匹配"的上界（ratio-test 很严：0.62 比值 + 绝对距离 0.5），
不等于"无覆盖"。唯一能定论的方式是**实机验证**。

**证据文件**（都在 `out/legacy-registration/`，`out/` 被 gitignore）：

- `avinoleum.json` / `blackshores.json` / `fabricatorium.json` / `jinzhou.json` /
  `laguna.json` / `lahai.json` / `qiqiu.json` / `tethys.json` — 八份配准报告
- `_unc.npy` — 未覆盖点的坐标集

**方法可信度已验证两次**：

- 泰缇斯之底：脚本算出 `3818 rows / 2059 inliers`，与它**已发布**的排除集完全一致。
- 拉海洛：脚本算出 `109,343 rows`，已发布的是 `109,540`，差 0.18%。

**"失败"的地区是有效信息，不是 bug**：时隙废都 / 梦州 / 冰原（0.6s）、黯原（25s）、
下层金库（4.1s）都报 `Insufficient descriptor registration support` 或
`No supported atlas component`，含义是**基础库里没有那片地形**。
判定逻辑在脚本第 74-75 行：`ids` 按该地区的坐标范围筛基础点，为空则匹配数 < 12。
注意梦州与今州**同属 frame 8 同一个平面**（梦州 x −10..−6，今州 x −1..12，互不重叠），
所以这不是坐标系问题。

---

## 四、眼下的任务：做"无基础库"构建给用户实机验证

**目标产物**：`out\map-test\IMao-WinUI.exe`，用户**以管理员运行**、游戏开着、16:9。
去掉基础库后去今州 / 拉古那等地判断定位是否照常。
若表现持平 → 基础库可直接删，程序包 981.9 MB → 831.6 MB，
连 `map-features` 包都不需要做。

**删两个文件是不够的。必须改四处，缺一不可：**

### 1. `IMao-Core/src/Feature/RuntimeFeatureRepository.cpp:183-191`

现在基础库缺失直接 throw：

```cpp
if (!sourceImfHashReady) {
#ifdef IMAO_ALLOW_XML_FEATURE_FALLBACK
    ... 退回 460 MB 的 Map_features.yml ...
#else
    throw std::runtime_error(failure);      // ← 发布配置走这里
#endif
}
```

快照模式下应**记录并继续**，不抛。

### 2. 同文件，`:206` 是**唯一**把 `visualIndexReady` 设为 `true` 的地方

```cpp
loaded->visualIndexReady = MapVisualIndexCodec::Load(... Map_visual_index.imx ...);
// :251 / :304 / :345 都只是设 false
```

无基础库时必须在**至少一个包分片合并成功**之后把它设为 `true`，
并令 `baseVisualTileCount = 0`。否则**定位会整体关闭**，即使包的索引都在。

### 3. `IMao-Core/src/Feature/LegacyFeatureExclusions.h:16` 与 `:21`

```cpp
:16  if (record.at("baseFeatureSha256") != Sha256Hex(baselineHash))
         throw std::runtime_error("Legacy feature exclusion baseline hash mismatch");
:21  for (const auto row : rows) if (row >= baselineRows)
         throw std::runtime_error("Legacy feature exclusion row outside baseline");
```

基础库不在 → `baselineHash` 为零、`baselineRows` 为 0 → **两个都会抛**
（第 21 行是任何 row 都越界）。需要在**无基线时安全跳过**。

注意：这个字段**只有拉海洛和泰缇斯之底**有，其余 11 个包在第 14 行就 return 了。

### 4. 测试树

- 移除 `Assets/FeaturesDatas/Map_features.imf` 和 `Map_visual_index.imx`
- **不要写** `Assets/Updates/baseline-files.json`

第 4 点的第二个动作是有意的：`ResourceSnapshotContext.cpp:159` 是
`if (fs::exists(baselineFilesPath))`，文件不存在就整段校验跳过，
从而绕过"基线清单列出特征 **或** 存在 `map-features` 包"这条规则，
**不需要再改校验代码**。

### 验证流程

```powershell
# 1) 重编
cmd /c "call C:\VSBuildTools-Current\VC\Auxiliary\Build\vcvars64.bat && cmake --build out\build\windows-x64-release --target IMao-CoreHost IMaoResourceSnapshotTests"
# 2) 测试（期望 58 checks, 0 failed）
x64\Release\IMaoResourceSnapshotTests.exe
# 3) 原生预检（见第七节的坑）
x64\Release\IMao-CoreHost.exe --check-resource-snapshot <绝对路径候选快照>
#    期望 {"error":"","resourcesReady":true,"viewportReady":true,"visualReady":true}
```

---

## 五、必须知道的坑

每一条都花过时间，重踩一次都不便宜。

### 运行时门禁

- **`CoreHostService.cs:104` 永远传 `--resource-snapshot`**，于是
  `RuntimeFeatureRepository.cpp:230` 会因**任何一个**已注册包未获运行时批准
  而中止整个资源加载，表现为 **"启动核心失败"**。
  场景批准在 `Assets/KuroMap/scene-validation.json`；`requiresGameValidation=true`
  的只有 **Darkplain / LowerVault / TimeRiftRuins**，这三个必须 `approved=true`。
  （踩过：包还没进源码时被 `Stage-UpdateResources.ps1:163` 静默跳过所以没事，
  一旦落地就上膛。）
- `KuroTileFeaturePack.cpp:188` 要求 `referenceVerification.passed=true`。
- 快照 strict 模式下，`ResourceSnapshotContext.cpp` 要求瓦片包的文件清单里
  **要么有源 XML、要么有 `features.imf`**。因为我们为瘦身删掉了 XML
  （2129 MB → 536 MB），这条被放宽过一次。

### 包与快照契约

- `ResourceSnapshotService.cs:194,203`：`map-data` 包**必须恰好一个**，
  且 `MapDataRoot` 等于它的目录。
- `UpdateStorage.VerifyDirectoryAsync` 会**枚举目录下所有文件**，清单外的文件直接
  throw（"资源目录包含清单之外的文件"）→ 包之间**不能嵌套、不能共用目录**。
  **这就是拆分包必须新增根字段的根本原因**，不是打包能解决的。
- bundled 快照里 `packages[].sha256` / `files` **故意是空的**，对应
  `FindBundledPackage` 的 A 分支（`ResourceSnapshotService.cs:208-212`）。
  **千万不要去填它们**：填了 A 分支被堵死、B 分支要求等于发布侧的 **zip 哈希**
  而内置侧没有 zip，于是客户端会把自己带的资源当成待下载更新。
- 发布侧 `ResourcePackage.Sha256` 是 **zip 的哈希**（见
  `Tests/ResourceUpdates/Program.cs:678`）。`tools/UpdatePublisher` 的 `prepare`
  在**文件表未变时沿用旧版本号 + 旧下载地址** —— 这就是增量机制，**包即为增量单位**。
- 运行时**没有**"包内文件级下载"，下载是整包 zip。

### Windows 与构建

- **大小写不敏感**：纯大小写的目录改名会被 git **就地记录**，索引里留下 PascalCase，
  大小写敏感的克隆会找不到目录而构建失败。修法：
  临时 `git config core.ignorecase false` → `git rm -r --cached <PascalCase>` →
  `git add <lowercase>` → 恢复。`git mv` 在 Windows 上做纯大小写改名会报
  `Invalid argument`，没用。
- `Stage-UpdateResources.ps1` **必须**在 Windows PowerShell 5.1 下跑
  （它会自我委派，且依赖 5.1 的 JSON 序列化保证字节一致），
  并且**必须对"重复构建到同一输出目录"保持幂等**（踩过 `Move-Item` 目标已存在的坑）。
- 程序包显式要求 `Assets/FeaturesDatas/Map_features.imf` 与 `Map_visual_index.imx`
  （`New-ProgramReleasePackage.ps1:101`）。想真正把它们移出程序包，必须改这里。
- WinUI 构建需要 `-r win-x64`，否则报
  `WindowsAppSDKSelfContained requires a supported Windows architecture`。

---

## 六、环境速查

| 用途 | 路径 / 值 |
|---|---|
| VS 工具链 | `C:\VSBuildTools-Current\VC\Auxiliary\Build\vcvars64.bat` |
| CMake 构建目录 | `out\build\windows-x64-release`（Ninja，增量快） |
| dotnet SDK | `tools\dotnet-sdk-8.0.424\dotnet.exe` |
| OpenCV | `IMAO_OPENCV_DIR=third_party\build\opencv-4.11.0` |
| Paddle | `IMAO_PADDLE_LIB=third_party\paddle-inference-3.0.0` |
| python | `C:\Users\Kahvia\anaconda3\python.exe`（numpy + scipy，**无 GPU 栈**） |
| GPU | RTX 4060 / 8 GB / 驱动 610.74 |

`Test-BuildPrerequisites.ps1` 需要上面两个环境变量，否则直接 FAILED。

### 配准脚本的代价模型

`Register-LegacySceneFeatures.py` 的耗时由**在图集里的空间重叠密度**决定，
不是由包的大小决定（不加 `--spatial-radius` 时走第 62-73 行的暴力批量路径，
`cdist(256, 全部包描述子)`，是矩阵乘级别）：

| 地区 | 关键点 | 图集行数 | 实测耗时 |
|---|---|---|---|
| 泰缇斯之底 | 7,291 | 3,818 | 5.3s |
| 隐海试验场 | 25,520 | 8,980 | 43.9s |
| 拉古那 | 72,054 | 34,657 | 405.5s |
| 今州 | 207,205 | 69,396 | 1,867s |
| 拉海洛 | 256,900 | 109,343 | 2,558s |

若以后要把这步常态化，**采样估计**（抽 ~2 万关键点、几个随机种子，几秒出带误差条的结果）
比重上 GPU 划算得多——GPU 只对第 62-73 行那段稠密距离有效，
后面的 ratio-test / RANSAC / 连通分量仍在 CPU。

---

## 七、预检的写法

`--check-resource-snapshot` 需要一个**绝对路径**的候选快照，
写法参考 `tools/UpdatePublisher/Program.cs:291-296`：

- `formatVersion: 2`、`bundled: false`（才会走 strict 校验）
- `baselineRoot` / `mapDataRoot` / `mapIconRoot`（若有）都要绝对路径
- 每个 package 要 `directory`（绝对）、非空 `version`、
  **64 位十六进制的 `sha256`**、以及**非空的 `files[]`**（strict 要求）

为了让预检通过，暂存树里还需要这些**非包**文件（真实构建由 `cmake/StageAssets.cmake`
拷贝整个 `Assets/` 提供，手工搭树时要自己补）：
`Map_features.imf`、`Map_visual_index.imx`、`IconTask_Features.yml`、
`IconWavePlateCrystal_Features.yml`、`candidate-packs.json`。

---

## 八、协作约定

- 用户要**实测**，不要空谈；结论要有可复现的证据。
- 用户会**抓错**，而且抓得准（本文档里好几处"踩过的坑"就是这么来的）。
  自己说错的地方要直接认，不要粉饰。
- 已经说过"不用再问"的事不要反复确认。
- **不能编译、不能通过预检的改动不算完成**。给不出验证就直说，
  不要留下一棵半改的树。
