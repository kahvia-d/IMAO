# 未完成项汇总（OpenWork）

> 这份清单把散落在**项目审计报告**、**帧率分析**和**更新系统方案**里的"没做完"集中到一处，
> 每条都带**优先级**、**出处**和**下一步动作**。
>
> 维护方式：做完一条就在该条上写明完成日期与验证方式，**不要删条目**（删了就不知道它曾经存在）；
> 新发现的未完成项加到对应分组。优先级沿用来源文档里的标注。
>
> 最后整理：2026-09-20（分片增量发布之后）。

## 1. 覆盖层与游戏帧率（最需要收尾的一块）

现象与成因**已查明但没有修**：玩家在 release 上感到的 120 → 99 fps 来自两件结构性的事——
一个**整屏置顶分层窗口长期在场**，把游戏从 Independent Flip 压回 DWM 合成；以及 **WGC 的整屏
GPU→CPU 回读**以游戏呈现节奏持续运行且不节流。出处：`GameFrameCostAnalysis_20260918.md` §5。

⚠️ 这块的结论**反复过**：DirectComposition 的两次"成立"测量**当晚即撤回**（§18），`2026.9.18.1`
那次发布也因此撤回。之后任何 DComp 结论都必须重新测量，不能引用旧结论。

| # | 优先级 | 未完成项 | 出处 | 下一步动作 |
|---|---|---|---|---|
| 1.1 | 高 | 覆盖层交换链是 `DXGI_SWAP_EFFECT_DISCARD`（blt 模型）+ `LWA_COLORKEY`；只画一小块的探针用 `FLIP_SEQUENTIAL` + DComp 只要 **1.8 fps**，真实覆盖层却要 **13–16 fps** | §16.2 / §16.3 | **一行配置改动**，先验证这条——目前最有价值的待验证项 |
| 1.2 | 高 | WGC 没有 `MinUpdateInterval`，整屏读回不节流 | §6.1 | 加 `MinUpdateInterval`（`ApiInformation` 运行时探测，旧系统回退），从源头砍流量 |
| 1.3 | 中 | 覆盖层窗口整屏大小，合成面积大；"无内容时隐藏窗口"没有真正生效 | §6.2 | 只覆盖真正要绘制的区域（注意：未必能恢复 Independent Flip） |
| 1.4 | 中 | 状态条每帧文本都在变，内容哈希几乎不命中 | §6.3 | 把每秒都变的文本排除出哈希，或让状态条降频更新 |
| 1.5 | 中 | 每帧一次 14.7 MB 的 `cv::Mat` 分配 + memcpy | §6.4 | 消费侧直接读 staging 映射视图，或复用缓冲，合并拷贝链 |
| 1.6 | 高（工作量最大） | 未用 DirectComposition 取代 `WS_EX_LAYERED` + `LWA_COLORKEY`——唯一有机会让游戏恢复 Independent Flip 的路 | §6.5 | 结构性改造；先按 §7 用 PresentMon 做「关/开/关」三段对比建立基线 |

## 2. 识别与资源

出处：`ProjectAudit_20260907.md` 的未完成表。

| # | 优先级 | 未完成项 | 下一步动作 |
|---|---|---|---|
| 2.1 | 高 | 928 个归档点位有 `floorId`，但运行时 `ItemDatas` 不保留楼层身份，定位/筛选没有完整楼层模型 | 把 state/country/floor/level 与坐标空间作为统一身份；用同一平面位置的不同楼层独立样本验证不串层 |
| 2.2 | 高 | 三个新区共 970 个点位，但 `scene-validation.json` 全部关闭，缺独立校准与可用特征包 | 收集校准锚点 → 构建并核验各场景特征包 → 实测后开放（不能只因 JSON 齐全就开启） |
| 2.3 | 高 | 旧 OCR 清单缺 61/61 张图，旧视觉清单缺 111/113 张图 | 可共享回归图归档到 `Tests`，补外部真值、反例与各场景覆盖 |
| 2.4 | 高 | 回放用例主要来自同一区域（大地图 8 用例含同图变体，不是 8 个独立场景） | 增加不同国家/场景、连续移动、传送、遮挡、缩放、开关地图、失焦与重新捕获的实机录像 |
| 2.5 | 高 | 全局定位 P95 **256.38 ms**、失败搜索可达 **5–7 s**、大地图首帧约 **4 s** | 分离可取消的快速候选搜索与分批后台恢复；复用已加载索引与区域结果；给首次定位/连续追踪/错误输入分别设延迟门槛（不放松几何要求） |
| 2.6 | 中高 | 点位/特征/标定/索引分散存放；同步 `-Apply` 验证后仍逐文件覆盖 | 按资源版本建立完整候选目录，全部文件与变换验证通过后统一切换，失败能回滚 |

## 3. 工程健壮性

出处：`ProjectAudit_20260907.md` 的未完成表。

| # | 优先级 | 未完成项 | 下一步动作 |
|---|---|---|---|
| 3.1 | 中高 | `App` 仍有多个工作线程共享状态；本轮只修了容器与视口读口，**未完成全类并发所有权证明** | 把一帧截图、帧号、UI 状态、视口与玩家定位发布为**不可变帧结果**，UI 与快捷键只消费快照 |
| 3.2 | 中 | 首页初始配置仍会设置默认开关/刷新间隔，功能页控件创建也可能发初始化事件 | 唯一配置状态放服务并用于界面回填；导航/重启不得覆盖用户选择 |
| 3.3 | 中 | 仍有历史坐标原点、比例和屏幕布局常量，无法从当前测试推出所有比例都正确 | 用明确坐标类型与场景标定替代重复换算；分辨率、DPI、UI 缩放与窗口比例分别验收 |
| 3.4 | 中 | 原生 `PrintWindow` 仍是同步系统调用；游戏挂起可能拖住停止 | 测试游戏挂起/退出/最小化与设备丢失；引入可取消的捕获边界与统一故障状态 |
| 3.5 | 中 | 多开程序仍可同时写同一账户文件；当前原子写不是跨进程合并协议 | 单实例约束，或按账户建立跨进程锁与事务存储 |
| 3.6 | 中 | `GetCurrentPath` 等仍依赖 ANSI/MAX_PATH；构建预设与平台声明不完全一致 | 明确只支持的 x64 发布组合（非 ASCII 路径的日志/诊断已单独修好，见下） |

**非 ASCII 安装路径（2026-10-07 已修）：**玩家把 IMAO 解压到 `E:\Games\明潮地图工具\` 这类目录时，
`stage=map-imf absent error=feature binary cannot be opened: <路径>` 里的路径是 `path.string()` 产出的
ANSI/GBK 字节，`StructuredLogger::Record` 用 nlohmann 的严格序列化写出这行日志时抛 `type_error.316`，
异常逃出日志函数被资源预加载的 catch-all 接住，于是**一条关于可选文件的告警把整个资源集变成不可用**。
修法分两层（`IMao-Core/src/Runtime/TextEncoding.h`）：

- 边界转换：所有以文本形式离开进程的路径改用 `Utf8Text()`（`u8string`），涉及 `FeatureBinaryCodec`、
`MapVisualIndex`、`LayeredFloorIndex`、`RuntimeFeatureRepository`、`CandidateFeaturePack`、`LayeredMapState`、
`ResourceSnapshotContext`、`Diagnostics::SessionDirectory`。
- 兜底：`StructuredLogger` 的日志/崩溃报告与 CoreHost 的 IPC/启动 JSON 改用
`DumpJsonText()`（`error_handler_t::replace`），任何非 UTF-8 字节只会被替换成 U+FFFD，**永远不会再抛**。
- 文件打开路径**故意保持 `path.string()`**（Windows 窄接口按进程代码页解释，换成 UTF-8 反而打不开）；
`fs::path` 重载的 `ifstream`/`ofstream` 走宽字符，本来就不受影响。

⚠️ 仍未覆盖：路径里含**代码页之外**的字符（例如中文系统上的日文/emoji 目录）时，OpenCV `imread`、
ImGui 字体/`imgui.ini` 等窄接口仍会失败或替换成 `?`；超过 `MAX_PATH` 的长路径同样没解决。回归测试在
`IMao-Core/tests/ResourceSnapshotTests.cpp`（`TestTextEncoding`）。

## 4. 更新系统（2026-09-20 分片发布后遗留）

| # | 状态 | 未完成项 | 下一步动作 |
|---|---|---|---|
| 4.2 | 待决策 | `runtime` 片是否拆成 `runtime-dotnet` / `runtime-native`（换 .NET/Paddle 时要下 142 MB） | 等下一次 .NET 或 Paddle 升级前定 |
| 4.3 | 待决策 | **跨渠道引用**：内容与某资源包完全一致的分片，能否直接沿用那个资源包的 URL | 纯发布侧改动；上传带宽吃紧时优先做 |
| 4.4 | 待决策 | 分片级断点续传（现在单片失败整次重试、从零） | 2026-09-21 实测：472 MB 离线包上传时 TLS 握手超时，重跑发布脚本按**已上传资产的名字与摘要**跳过、只补缺的那几个，所以资产级续传已经有了，缺的是单个大文件内部的字节级续传。看用户实际失败率再定 |
| 4.6 | 新发现 | 安装根目录与快捷方式的图标**不会**随更新变化（更新器按设计不碰原始副本，见 `ProgramUpdates.md`） | 要么接受，要么给启动器做"下次启动替换自己"的自更新（改的是救援路径，需单独设计与测试） |
| 4.7 | 小 | `IMao-WinUI/Package.appxmanifest` 引用了 5 个不存在的 PNG（`StoreLogo.png`、`Square150x150Logo.png` 等） | 只在打 MSIX 包时会踩；补资源或清理清单 |

已了结：4.1（**构建不可复现**）——**2026-10-07 已修**。native 编译/链接加 `/d1trimfile:<仓库根>`，用
`-DCMAKE_C_FLAGS`/`-DCMAKE_CXX_FLAGS` 覆盖时显式带回 MSVC 默认开关，托管关闭 `CsWinRTAotOptimizerEnabled`；
两次独立云端预演（`37582143673`／`37582156783`）产出同一个安装 ZIP `d3586ab0…`，`2026.10.7.1` 的正式构建
沿用同一份字节。预期每版必下的地板由 `core` + `ui` ≈56 MB 降到只有 `ui` ≈30 MB（逐片对照留待下一版实测）。
⚠️ 跨 Runner 镜像的 `?A0x…` 差异仍未解释，换镜像后要重新核对。

已了结：4.5（下一版发布带完整 zip）——**2026-09-21 发布的 `2026.9.21.1` 已带上**
`IMao-v2026.9.21.1-windows-x64.zip`（717.3 MB），并因为 `map-data` 有变化同时带上 472.1 MB 离线集合包；
分片只上传变化的 `core` / `ui` / `assets-map-data` 三片加描述符，另外四片（`runtime`、`assets-misc`、
`assets-map-icons`、`assets-tiles`，合计 646.7 MB）沿用 `2026.9.20.2` 的 URL。

## 5. 代码里的 TODO

| # | 位置 | 内容 | 建议 |
|---|---|---|---|
| 5.1 | `IMao-WinUI/AppNotificationActivationHandler.cs:29,46`、`AppNotificationService.cs:34,47` | WinUI 模板遗留的"处理通知激活"占位（命中会弹 TODO 对话框） | 这个程序不用 app 通知 → **直接删掉这段死代码** |
| 5.2 | `IMao-WinUI/ShellPage.xaml.cs:16,36` | 模板文案：导航标题/图标、标题栏图标 | 标题栏图标运行时已由 `MainWindow.xaml.cs:23` 正确设置；清掉过期注释即可 |

## 6. 环境与流程（提示，不是必须做）

- 生产私钥唯一副本曾只存在于 Codex 包的 `LocalCache` 里；**2026-09-20 已复制回文档主位置**
  `%LOCALAPPDATA%\WWMAP-TOOLS-Publisher\release-signing-key.json`（两份 SHA-256 一致、DPAPI 当前
  用户可解封、与 `Assets/Updates/trusted-keys.json` 匹配）。原始那份仍在，可视为备份。
- 发布后本地 `main` 会落后远端（脚本用 GitHub API 直接写远端 `main`）：先 `git fetch origin`，再
  `git merge --ff-only origin/main` 追平。**不要用 `git fetch origin main:main`**——`main` 已检出时 Git
  直接拒绝（`refusing to fetch into branch ... checked out at ...`），2026-09-21 踩过一次。

## 8. 分发包体积（2026-10-10，核实群友反馈后）

群友报了三条重复，**数字全部属实**，核实后两条已改、一条不能改。原始数字：

| 他说的 | 核实 |
|---|---|
| ① 302 张 itemImages PNG = 23.8 MB、5 份 itemsData JSON = 8.4 MB 内嵌进 `.rsrc` | ✅ 23.77 + 8.39 = **32.16 MB**；CoreHost 35.72 MB 里 `.rsrc` 正是 **32.19 MB** |
| ① 发行包又单独发 KuroMapIcons 35.5 MB | ✅ `Assets/KuroMapIcons` = **35.53 MB / 541 文件** |
| ① `310000620` 两份字节相同 | ✅ 而且不止它：**286/301 相同**，15 个已漂移 |
| ② Launcher 段 9.3 MB / 文件 65 MB / overlay 55.6 MB | ✅ **9.38 / 64.99 / 55.61** |
| ② Bridge overlay 56.2 MB | ✅ **56.16**（段 9.19，文件 65.35） |
| ② root 另有一份 .NET 约 67 MB | ✅ 实测 **70.51 MB**（180 个文件） |
| ③ itemsData_World 既内嵌又单独发布 | ✅ 而且**内嵌那份在任何正常安装里都读不到** |

### 已了结：8.1（内嵌图标与场景点位）

**它不只是占磁盘，是每版重下。** `ShardMap.cs:32` 把 CoreHost 放在 `core` 分片，而
`ProgramUpdateStore.cs:190-213` 的规则是「分片里**每一个**文件都和上一版签名清单一致时才跳过」——
只要一个文件变了就整包重下。CoreHost 每版必变，于是那 22.5 MB **永不改变的已压缩 PNG**
每次都要陪着重下一遍。

**改法：**

- `IMao-Core/src/IMao-Core.rc` 删掉 301 个 PNG 与全部 5 个 JSON 资源，**只留 `IDB_PNG_Activity_02`**；
- `DrawItemBase.cpp` 删掉 `LoadJson`（唯一调用点就是那个回退）与内嵌回退分支，缺文件即报错；
- `CMakeLists.txt` 删掉那段已失效的 JSON `OBJECT_DEPENDS`（`Resource/*` 的全量 glob 本来就在下一行）；
- 删掉 `IMao-Core/src/Resource/itemImages/` 下 301 个已无人引用的文件（全库引用核查过：只有 `.rc`
  和 `resource.h` 提过 `itemImages`）。

**为什么单留 `Activity_02`：** 它是唯一一个「两处都没有就没人管」的 id——只被**筛选目录**
（`catalog-*.json`，副本挑战「深坠异想奇境」）引用，而 `Sync-KuroMapData.ps1:213` 从**点位数据**收集
图标来源，所以任何一次同步都不会给它在 `icon-manifest.json` 里留位置；手改清单也会被下次同步当孤儿删掉。
它那张图和 `Activity_02_1` 的 `icon-0175.png` **字节完全相同**（6498 字节），所以留着它就是留住现状。
其余 301 个 id 要么有发行图标，要么和另外 **232 个 catalog id** 一样本来就没有图标。

**实测（同一份源码、同一次构建）：**

| | 改前 | 改后 |
|---|---|---|
| `IMao-CoreHost.exe` | 35.72 MB | **3.54 MB** |
| 其中 `.rsrc` | 32.19 MB | **7 KB** |
| `core` 分片（CoreHost + common.dll）zip | 26.13 MB | **2.03 MB** |

即**每个玩家每次 core 分片变动的更新少下约 24 MB**，装完少占 32 MB。

⚠️ 顺带确认了一件容易被误读的事：`.rc` 里的资源标识符来自 `resource.h`，而那里写的是
`constexpr auto IDB_PNG_x = 130;`——**RC 编译器不认 `constexpr`，所以这些在 `.rc` 里是字符串名**，
正好和 `FindResource(..., L"IDB_PNG_" + itemId, L"PNG")` 对得上。rc.exe 会把名字转成大写存，而
`FindResource` 的名字比较不区分大小写——这条我直接用 `LoadLibraryEx` + 枚举/查找实测过。

### 已了结：8.2（启动器与桥接的单文件压缩）

两个程序都是 `SelfContained` + `PublishSingleFile`，overlay 里各装着一整套 .NET 运行时，
而且都被 `ShardMap.cs:24-28` 归入 **`ui`** 分片——**每版必变**的那个。

改法只有两处：`ProgramLauncher.csproj` 加 `<EnableCompressionInSingleFile>true</EnableCompressionInSingleFile>`，
`IMao-WinUI.csproj` 发布桥接的那条 `Exec` 加 `-p:EnableCompressionInSingleFile=true`。

| | 改前 | 改后 |
|---|---|---|
| `IMao-Launcher.exe` | 64.99 MB | **33.93 MB** |
| `KuroSyncBridge.exe` | 65.35 MB | **33.84 MB** |
| 启动耗时（桥接无参返回路径，9 次中位） | 75.9 ms | 129.8 ms（**+54 ms**） |

⚠️ **但它省的是磁盘，不是流量。** 发布端用 `CompressionLevel.Optimal`（`UpdatePublisher/Program.cs:455`），
**分片 ZIP 本来就把这 130 MB 压到 60.58 MB 了**。把压缩版换进 `ui` 分片重新打包：**60.58 → 59.34 MB**，
只省 1.24 MB。我一开始以为压缩能省下载，**是实测把这个推断推翻的**。

### 8.3 不能做的三条（连原理一起记下）

- **根目录那份 70.51 MB 的 .NET 运行时不能去。** `Build-ReleaseCandidate.ps1:118` 用
  `--self-contained true` 发布主程序；改成依赖框架就要玩家先自己装 .NET 8 Desktop Runtime，
  对一个「解压即用」的工具是倒退。
- **启动器不能改成依赖那份运行时。** 它的职责是「程序坏掉时把它修起来」
  （`ProgramLauncher.cs`：健康检查、失败回退到上一版），依赖根目录那份运行时就等于在最需要它的时候失效。
  **这是设计约束，不是疏忽。** 桥接改成依赖框架技术上更可行（根目录那份永远在），但要多一条
  「浏览器启动时怎么找到运行时」的路径，为磁盘数字换耦合不值。
- **NativeAOT（彻底不要运行时）不是开关，是一个项目。** `IMao-WinUI.Core/Updates/` 下的 JSON
  全是反射式 `JsonSerializer.Deserialize<T>`（21 处，**没有任何 source generator**），要 AOT
  得先把这层改成源生成序列化——而那是在**救命路径**上动刀。

### 8.4 核实中发现的一处文档与实测不符

本文件 4.1「已了结」段里写着每版必下的地板是 `core` + `ui` ≈ **56 MB**、修好后只剩 `ui` ≈ **30 MB**，
并注明「逐片对照留待下一版实测」。我按当前分片实测：

```
core 26.13 MB + ui 60.58 MB = 86.7 MB   （文档说 56 MB）
ui 单独                     60.58 MB   （文档说 30 MB）
```

**比文档高约一倍**，而且那句「留待实测」至今没做。差异最可能来自 launcher/bridge 长成两个 65 MB 的
自包含单文件（它们就在 `ui` 里：130.34 MB，占该分片原始体积 134.14 MB 的 97%）。**这条仍待重新推导。**

### 8.5 验证状态

- 构建与链接通过（`LoadJson` 若还有引用会链接失败，没有）。
- 资源目录实测：改后 `.rsrc` 只有 2 种类型（`PNG` 按名字 + `RT_MANIFEST` 按整数），`PNG` 下**唯一一条**
  `IDB_PNG_ACTIVITY_02`；用真实的 `FindResource(L"IDB_PNG_Activity_02", L"PNG")` 命中，6498 字节。
  同一个调用打在不存在的名字上返回 not found，说明这个测试本身有效。
- 资源更新套件 **114 项全过，0 失败**，区域选择 `filtered snapshot ready=True`，发布器 34 项通过。
  （114 是 `main` 的基线：4.11 的 3 项与 4.12 的 2 项在 `codex/program-update-disk-reuse` 上，119 = 114 + 5。）

**云端预演 run `38044772630`** —— 2026-10-10，`publish=false`，**success**，28 分 56 秒，SHA
`d0d28a6ab0718742758784b1aa6a6510c2925cba`。这次落地成了一个**受控 A/B**：上一趟预演
（`38029179059`）产出的是同一套工件，只差 8.1 / 8.2 这两处改动。

| 分片 | 改前 | 改后 | 变化 |
|---|---|---|---|
| **core** | 26.08 MB | **1.98 MB** | **−24.11 MB** |
| ui | 58.80 MB | 59.02 MB | **+0.22 MB** |
| 其余五个 | 不变 | 不变 | 0 |
| **合计** | 438.57 MB | **414.68 MB** | **−23.89 MB** |

- 8.1 的节省在云端复现：本地算的 `26.13 → 2.03` 对云端 `26.08 → 1.98`，差 0.05 MB。
  **这个数字本身就是 CoreHost 真的瘦下来的证据**——`core` = CoreHost + `common.dll`，
  一个 `.rsrc` 里塞着 32.19 MB 不可压缩 PNG 的 CoreHost 不可能压进 1.98 MB。
- 8.2 的判断也被云端证实：`ui` **反而涨了 0.22 MB**（预压缩的 bundle 让 ZIP 压得更差一点），
  与「只省磁盘、不省流量」一致。要是压缩真能省下载，这里该掉 30 MB 左右。
- 其余验收行：`PASS complete resources, isolated IPC snapshot identity/shutdown, packaged native picker,
  and unchanged package contents.`、`Clean-source candidate complete … source d0d28a6a…`、
  `Program update checks: **52** passed`、`Resource update checks: **114** passed, 0 failed`。
  **52 与 114 都是 `main` 的基线**（52 + 6 = 58、114 + 5 = 119 是另一条分支上的数字）；
  这里出现 58 / 119 才说明基线串了。

⚠️ **两件这次仍然没被覆盖的事：**

1. **端到端那条链还是断的。** 把 2846 行预演日志搜遍，**没有一处出现 `--pipe`**——没有任何一步走到
   `CoreHostMain.cpp:859` 的 `Initi()`，而点数据加载就在那里；`Test-ProgramReleasePackage.ps1` 走的是
   `--check-resource-snapshot` 的**早退分支**。所以真实的点加载路径目前只由「改动前后调用同一个函数、
   传同样的参数，差别只在文件缺失时」来保证。**要一次真机启动才算完。**
   本地也试过独立起 CoreHost，四次都被快照校验挡在 `Initi()` 之前（新旧两个二进制表现完全一致，
   所以是调用方式不对，不是改动引起的）。
2. **「压缩在云上生效」是推出来的，不是看见的。** CI 日志不打印启动器体积，`rehearsal-report.json`
   的 `program.shards[].files` 只有计数没有尺寸。依据是 csproj 属性随提交进了 CI，
   而 `Build-ReleaseCandidate.ps1` 不覆盖该属性。

⚠️ **这个 SHA 的预演对正式构建的有效期：** 任何新提交都会移动 `build-info.json` 里的 `sourceCommit`
与 `sourceTreeSha256`，因而改变 `ui` 分片——**哪怕只改 `Docs/`**。`Docs/` 本身不进发行包
（`Test-Path out\map-test\Docs` = False），但 `New-ProgramReleasePackage.ps1:98-99` 会把
`Docs/ResourceUpdates.md` 与 `Docs/ProgramUpdates.md` 复制进包当 `README-Updates.md` / `ProgramUpdates.md`；
`OpenWork.md` 不在被复制的名单里，所以改它不动任何发行字节——**但提交本身会**。
**正式发版必须在发版的那个 SHA 上重新预演。**
