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

已了结：4.8（**版本目录没有 GC**）——**2026-10-10 已修**。`ConfirmHealthyAsync` 在状态提交落盘之后调用
`PruneUnreferencedVersions`（`ProgramUpdateStore.cs`），删除 `versions/` 下既非 `Current`、也非 `Previous`、
`Pending`、`Trial` 的目录。安装根目录不在 `versions/` 之下，所以永不参与——它仍是最后回落的那一份。
清理是尽力而为的：被扫描器、索引器或尚未退出的子进程占用的目录留到下一次提交，失败一律吞掉，绝不让启动失败；
`versions/` 或某个版本目录是符号链接/目录联接时整棵不动。候选目录来自目录列举、按**名字**与状态比对，
`keep` 集合用 `OrdinalIgnoreCase`——**不用任何状态值拼路径**，因此损坏或恶意的版本 id 无法操纵删除目标。
`state.Versions`（防重放记录）与目录生命周期无关，保持不动。
三条新用例覆盖"提交后清理无人引用的版本"、"四个状态槽各自保留"、"残留被占用时不阻断提交"；
把调用去掉做对照，正好是 `the version no record names is gone` 一条变红。既有 52 项一并通过（合计 55）。
⚠️ 存量安装已经积下的孤立目录要等**下一次成功更新**提交时才被收走（本机那份 6.17 GB 是手工清的）；
一并完成了 [`ProgramUpdates.md`](ProgramUpdates.md) 里「不自动清理」那段的改写。

已了结：4.9（**复用是"复制"而非"共享"**）——**2026-10-10 已修**。`TryReuseVerifiedAsync`
（原 `TryCopyVerifiedAsync`，`ProgramUpdateStore.cs`）改为**先建硬链接、失败则复制**
（`UpdateStorage.TryHardLink` 包 `CreateHardLinkW`，任何失败都返回 false 而不抛异常），
两条路径落地后都对**目标文件**算一次 SHA-256 与签名清单比对——哈希仍然只承担一个职责：
让一个本地损坏的文件只拖累它所在的那一个分片。真正保证发布物可信的是 `PrepareAsync` 在装配之后对
**整棵树**跑的 `VerifyDirectoryAsync`，所以链接不可能把一个副本会拦下的字节带过发布闸门。
判定复用哪些文件不变（仍由两份签名清单比 size + SHA-256 得出），因此**不需要任何"冻结层/资源"分类表**：
被链接的集合自动等于"这次没变的那些文件"。
语义锁写在 `UpdateStorage.RejectLink` 的文档注释里：**判据是 `FileAttributes.ReparsePoint`，硬链接不是
reparse point，更新系统有意使用它；不要收紧成链接计数**——已更新过的安装按设计就持有共享文件，
那条检查会让它们全部无法启动且无法远程修复。
三条新用例：共享文件链接数为 2 而改动的文件为 1（用 `GetFileInformationByHandle` 读，共享与复制的大小
摘要完全一样，只有链接数能区分）；持共享文件的版本仍通过 `ValidateInstalledAsync`（启动器真正跑的校验）
且已链接文件能过 `RejectLink`，同时目录联接仍被拒绝（junction 无需提权即可造）；`TryHardLink` 的
失败契约。把建链改成恒复制做对照，失败信息正是 `an unchanged file is one file with two names (links=1)`。
合计 **58 项通过**。
⚠️ **实机节省量还没实测**——套件只能证明链接确实建立、链接数为 2、共享的树仍能通过启动校验；
逐字节重复量（本机 1,914 MB）是在改动前的三棵树上用只读脚本量出来的，下一次真实更新后才能对账。
⚠️ 跨卷回退没有自动化用例（套件只有 C: 一个卷），回退是一条 `if`，由上面那条契约用例与"源被损坏"
用例共同覆盖。一次性迁移**决定不做**，理由见 [`ProgramUpdateDiskReuse_20261010.md`](ProgramUpdateDiskReuse_20261010.md) §6.4。

已了结：4.10（**启动器与桥接没有做路径归一化**）——**2026-10-10 已修**。原先判断"4.1 可能未覆盖这两个产物"是对的，
而且原因很具体：4.1 的 `-p:ContinuousIntegrationBuild=true` 只传给了 WinUI 的发布（`Build-ReleaseCandidate.ps1:120`），
**启动器的发布（同文件第 126 行）没传**；桥接则是在 `IMao-WinUI.csproj` 里用一个 `<Exec>` 起**独立的
`dotnet publish` 进程**构建的，而命令行的属性**传不进子进程**（属性不是环境变量）——所以两个产物都没拿到
`DeterministicSourcePaths`。
后果不只是"字节会变"：PDB 记录里的**绝对检出路径参与编译**，进而决定程序集的 **MVID** 与单文件包里
**嵌套映像的确定性时间戳**。实测 10.4.2 的产物里写着 `C:\Dcode\WWMAP-TOOLS\tools\...`，10.7.1 的写着
`C:\a\IMAO\IMAO\tools\...`——两台机器的检出路径各自留在了自己的发行字节里。
修法：两处各补 `-p:ContinuousIntegrationBuild=true`。验证方式是同一个目录、同一个提交只差这个开关：
启动器的 `C:\Dcode\WWMAP-TOOLS` 出现次数 **1 → 0**、PDB 路径变成 `/_/tools/ProgramLauncher/...`、
`/_/` 计数 62 → 63、两次产物只差 205 字节（含 MVID 与嵌套时间戳）；桥接 **2 → 0**、`/_/` 63 → 65。
`IMao-WinUI.csproj` 的 XML 与 `Build-ReleaseCandidate.ps1` 的语法都已校验。
⚠️ 这条修的是**可复现性**（本机构建与云端构建同提交应逐字节相同，这正是云端预演要保证的，
而 `New-ProgramReleasePackage.ps1` 只拿同一份构建自己的回执去核对启动器，查不出这件事）。
它**不会**让这两个文件变成可共享，也**不改变每版落盘量**——磁盘那一半是 4.13，而且我原来的预估是错的。

已了结：4.11（**资源暂存目录残留**）——**2026-10-10 已修**。根因不是"忘了清理"：`InstallReleaseAsync` 的清理写在
`finally` 里，覆盖了它能看到的所有退出路径，唯独覆盖不了它看不到的那一种——进程被玩家关窗、被启动器的进程组结束、
或断电时，那个 `finally` 根本不会执行；而 `staging/` 除此之外没有任何地方会看，每个后续事务也只清自己那一个目录，
所以一次中断就是永久残留。本机那份 69.6 MB 里的两个包**都已经解压并安装到位**（残留里只有 zip、没有 `unpacked`），
恰恰说明工作早就完成了，只有现场没收拾。
修法：`ResourceSnapshotService.InitializeAsync` 在**取得与事务相同的锁之后**扫一遍 `staging`。锁是这件事安全的前提——
`staging` 只由事务写，而每个事务都持这把锁，所以此刻扫到的一定是遗留物，可以整块删。
尽力而为（被占用就留到下次启动，绝不阻断启动），`staging` 本身或其下某个条目是符号链接/目录联接时整块不动。
三条新用例：被中断留下的暂存目录在下次启动被清掉；被别的进程占住时不阻断启动且留到下次；
`staging` 是联接时**不穿透删除**它指向的内容（junction 无需提权即可造）。
把清扫调用去掉做对照，正好是第一条变红。资源更新套件当时 **117 项通过**（4.12 又加两条后为 119 项），
`ResourceUpdates.md` 的对应说明已改写。
⚠️ 存量残留要等**下一次启动**才被收走（本机那份 69.6 MB 目前还在）。

已了结：4.12（**资源描述文件缓存无限累积**）——**2026-10-10 已修**。先量清了两个目录，因为它们的结论相反：
`packages/` **不按版本累积**，它是每个包 ID 一个目录、原地替换（`map-data` 68.1 MB / `map-icons` 35.5 MB /
`tethys-kurotiles` 是 0 文件的空壳），所以那 103.6 MB 是**当前**集合；真正无限累积的是 `snapshots/`。
那里有三种文件，只有一种算记录：`v2/<快照标识>.json` 是 `StageAsync` 落下的真快照，**故意保留全部签名包**
以便玩家删掉某地区后还能重新启用，状态文件也引用它——**永不清理**；而 `bundled-<哈希>.json` 与
`runtime-<哈希>.json` 是缓存，两者都只在"按内容算出的名字不存在"时才写，缺了下次启动自动重建。
实测那个安装积了 **218 个**（bundled 92 + runtime 126，9.3 MB），而 `activation.json` 只引用 **1 个**，
新增速率约 9.5 个/天。修法：`InitializeAsync` 在**持锁**时、且在状态确定之后，删掉既不被
`ActivePath`/`PreviousPath`/`PendingPath`/`Attempt.SnapshotPath` 引用、又不在"最新 8 个"之内的描述文件；
`v2/` 是子目录，`EnumerateFiles` 本来就不递归。尽力而为，被占用的留到下次。
两条新用例：无人引用的被清、被引用的与 `v2/` 一定留下（最老一批必删、最新一批必留，中间一段**刻意不断言**——
它取决于本次启动自己写了几个）；被别的进程占住时不阻断启动。资源更新套件 **119 项通过**。
`ResourceUpdates.md` 里那句玩家承诺**没有被削弱**——它说的是 `v2/` 与 `packages/<包 ID>` 保留已安装资源，
本次清的只是可按内容重建的缓存，文档已把这个区别写明。

已了结：4.13（**两个 65 MB 产物的版本戳**）——**2026-10-10 已做**。事实：`IMao-Launcher.exe` 与
`KuroSyncBridge.exe` 跨版本**只差几百字节，且全部是版本戳**。启动器带的是 PE `FileVersion` = 程序版本
（`New-ProgramReleasePackage.ps1` 还强制校验相等），桥接带的是 SDK 自动追加的源码修订号；
另外桥接**引用** Core 工程，而 Core 同样导入 `Version.props`，所以它内嵌的那份 Core 也带着程序版本——
**只去掉桥接自己的戳是不够的**。
改法四处：`Version.props` 新增 `IMaoLauncherVersion` 与 `IMaoCoreVersion`（各 `1.0.0`，注释写明改启动器协议时
必须同时改前者）；启动器与 Core 用各自那一行覆盖 `Version`/`AssemblyVersion`/`FileVersion`；
三者都关掉 `<IncludeSourceRevisionInInformationalVersion>`；`New-ProgramReleasePackage.ps1` 那条校验改为核对
`IMaoLauncherVersion`——**把这个启动器绑到本发行版的仍是回执的 provenance 与摘要**（同文件上游三行），
版本字符串不是绑定手段。桥接本身没导入 `Version.props`，所以只加了关戳那一行。
判据是"换一个 `IMaoVersion` 重新构建，字节是否不变"：实测在 `2026.10.8.1` 与 `2099.1.1.1` 两个完全不同的
程序版本下，启动器与桥接产出**逐字节相同**的产物，版本信息显示 `1.0.0` 且不再带 `+提交`。
**没动更新子系统源码的发版，这两个产物每版落盘从约 190 MB 降到约 0**——这正是我原先以为靠"修好可复现性"就能拿到、
后来发现拿不到的那部分。
⚠️ 代价要说清：两个二进制不再自带源码出处（`build-info.json` 与 `launcher-build-info.json` 仍带）；
启动器 PE 版本从此是"启动器版本"而非"程序版本"，看文件属性时别误读。`ProgramUpdates.md` 已同步。
⚠️ **收益的边界（2026-10-10 云端预演后更正）**：上面那句原来写成"每版落盘降到约 60 MB"、并且
"逐版相同成立"，**都不准确**。启动器是按源码 glob 编译 `IMao-WinUI.Core/Updates/*.cs`（见
`ProgramLauncher.csproj`），桥接引用 Core——**所以只要更新子系统的源码变了，这两个产物必然变字节**，
与版本戳无关。真实规律是：

| 发版 | 启动器 / 桥接 | 该版为它们落盘 |
|---|---|---|
| 装了本次改动的这一版（动了 `Updates/*.cs`） | 与上一版**必然不同** | 仍写约 130 MB |
| 之后没动更新源码的版（UI / 地图 / 资源类） | 与上一版**逐字节相同** | **共享，约 0** |

即**收益从"再下一版"开始显现**，而大多数发版不动 `Updates/*.cs`，所以稳态下这两个 65 MB 基本不再重复。
剩下的每版 churn 是 `IMao-WinUI.dll`（自带程序版本戳，约 1.5 MB）加上真正改动过的文件。
（`ProgramUpdateDiskReuse_20261010.md` §5.4 把「自有产物 190 MB/版」当作稳态模型参数，那份按
`Docs/README.md` 的约定是**不再追改**的当天记录——以本条为准。）
⚠️ 次要边界：实测同一份源码在**不同输出布局**下构建出的桥接仍差 173 字节（MVID / PDB GUID / Roslyn
确定性哈希 / 嵌套 PE 时间戳）——**不是版本泄漏，是构建布局参与了编译**（同一布局下换 `IMaoVersion`
构建则逐字节相同，那才是判据）。CI 布局是固定的（`New-CiReleaseArtifacts.ps1:13` 把
`$GITHUB_WORKSPACE/out/ci-release` 交给 `Build-ReleaseCandidate.ps1`，内部 `publish` / `managed-build` /
`launcher` 与 `x64/Release` 全是固定子路径，版本号只用在编译之后的
`program/IMao-v<版本>-windows-x64` 目录名上）。但**工作区路径本身变过**：预演日志是
`D:\a\IMAO\IMAO`，而 `2026.10.7.1` 的启动器里嵌的是 `C:\a\IMAO\IMAO`。这条路径差异现在已被
`ContinuousIntegrationBuild` 消掉（产物里只剩 `/_/`），剩下的布局影响与 4.1 里那条未解释的 `?A0x…`
同类，换 Runner 镜像时要一起核对。

**本组（4.8–4.13）的端到端验证**：云端预演 run `38029179059`，SHA `574803a6e90052907b0504f20988036f9e925431`，
2026-10-10，**success**，28.8 分钟，`publish=false`。它跑到了我改动的每一处：启动器在
`out/ci-release/launcher` 建成、`Clean-source candidate complete` 落在这个 SHA 上、
`New-ProgramReleasePackage.ps1` 的新校验通过、紧接着 `Test-ProgramReleasePackage.ps1` 验收了刚产出的包
（`PASS complete resources, isolated IPC snapshot identity/shutdown, packaged native picker, and unchanged
package contents.`），`Program update checks: 58 passed.` 与 `Resource update checks: 119 passed, 0 failed.`
与本机数字完全一致。⚠️ **任何新提交都会让这个 SHA 的预演对正式构建作废**——正式发版要重新预演。

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

## 7. 检测器迁移（2026-10-08 的 SURF → SIFT 决策）

出处：[`DetectorMigrationDecision_20261008.md`](DetectorMigrationDecision_20261008.md)（当天记录，按
`Docs/README.md` 的约定不再追改）。该文第五节把 `fabricatorium`（隐海试验场）的 **4/12** 记为「明确的异常，
未解释」，并在第七节把它列为**不迁移 SIFT 的理由之一**（原话：「在解释它之前不该迁移」）。
**2026-10-10 查清：那条理由不成立**，而且原文对它的三处描述都是错的。

| # | 事项 | 现状 | 备注 |
|---|---|---|---|
| 7.1 | 词汇表去重（14 份相同拷贝改为共享一份，省 26 MB） | 未做 | 与检测器无关，风险最低、收益最大 |
| 7.2 | 查清 fabricatorium 为什么一半查询零匹配 | **已了结 2026-10-10** | 见下 |
| 7.3 | histogram 压缩（17.56 MB 稀疏 TF-IDF 换窄类型，省 5–9 MB） | 未做 | 要重新验证检索排序不变 |
| 7.4 | 公平的 SURF↔SIFT 对照 | **覆盖面对照已完成**；定位精度对照仍缺 | Python wheels 没有 SURF，要 C++ 侧探针 |
| 7.5 | 换检测器 | 未做，**理由从三条减到两条** | 「收益 2.4x 不是 8x」与「缺公平对照」仍然成立 |

### 已了结：7.2（fabricatorium 的 4/12）

原文三处描述都错：

| 原文 | 实际 |
|---|---|
| 「它是一张大的室内工业图」 | 反了。**36.5% 的像素是黑色虚空，73.3% 的面积在 184px 窗口下是平的**（局部 std < 8）。它主要是大片海面加十来个孤立的小结构 |
| 「可能是重复纹理导致比率检验把匹配全滤掉了」 | 反了。**根本没有匹配可滤**——`good=0` 是两边都几乎没有特征点，不是匹配被筛掉 |
| 「它有 5 个查询一个匹配都没有」 | 是 **6** 个；另 6 个是 5/7/13/55/65/85（原文写「另 7 个」，两数都对不上） |

**真机制：通过与否完全由「裁剪框里有几个参照特征点」决定。** 把当时那 12 个 case 按同一颗种子复现：

| # | 框内参照点 | good | 结果 | | # | 框内参照点 | good | 结果 |
|---|---|---|---|---|---|---|---|---|
| 0 | **76** | 57 | ✅ | | 8 | 6 | 5 | ❌ |
| 2 | **21** | 13 | ✅ | | 10 | 10 | 7 | ❌ |
| 5 | **99** | 85 | ✅ | | 1 / 3 | **1** | **0** | ❌ |
| 6 | **89** | 65 | ✅ | | 4 | 4 | **0** | ❌ |
| | | | | | 7 / 9 / 11 | **0** | **0** | ❌ |

通过的 4 个，框内参照点全部 ≥21；失败的 8 个全部 ≤10，其中 **7 个不足 8**——而 8 正是判据要求的
内点数。**0 个参照点的地方，任何检测器都凑不出 8 个内点。**

### 根因：那个门槛在数学上就分不开

原门槛是「不黑 **且** `std ≥ 18`」。问题在 `std` 量的是**亮度离散度**，不是**特征内容**：平滑的海面渐变
轻松超过 18，却一个角点都没有。实测三个包的分布：

| 包 | 空白裁剪的 std p90 | 有内容裁剪的 std p10 | 能否分开 |
|---|---|---|---|
| **fabricatorium** | **16.7** | **11.4** | **完全重叠——分不开** |
| mengzhou | 13.1 | 16.7 | 勉强 |
| tethys | 5.5 | 12.0 | 分得很开 |

**这才是隐海独有的原因**：它的「空白」是**平滑的灰海面**（std 顶在门槛下沿），而泰缇斯的「空白」是
**接近全黑的虚空**（std 远低于门槛）。同一个门槛，对一种地图有效，对另一种失效。

### 改法：把门禁换成可验证的判据，并且把「这题有没有答案」记进报告

`measurements/accuracy-check/imao_vs_sift.py` 新增 `--gate`：

- `art`（**默认，原样保留**）——2026-10-08 的协议，**旧数可逐包复现**；
- `texture`——改用 `mean|Laplacian| ≥ 6.0`（常量 `TEXTURE_GATE`，2026-10-10 用三个包各 400 个候选标定）；
- `answerable`——**参照集自己必须在框内有 ≥8 个关键点**。这是唯一能保证「题目有答案」的判据：
  三个纹理代理（std、`mean|Laplacian|`、角点像素数）都标定过，**没有一个能干净分离**。

报告新增 `queries[]`（每个裁剪的位置、`ink`、`std`、`meanLaplacian`、`referenceKeypoints`），
`sweep_per_tile.ps1` 新增 `-Gate` 与三列汇总（`unanswerableQueries` / `referenceKeypointsMin` /
`referenceKeypointsMedian`）。**「这题有没有答案」从此是报告的一部分，不是事后再查。**

### 14 包实测（每包 12 次查询，`sift-friend`，逐瓦片参照）

| 包 | 原门禁 | 其中无答案 | 纹理门禁 | 其中无答案 | 有答案门禁 | 覆盖率 SIFT | 覆盖率 SURF |
|---|---|---|---|---|---|---|---|
| tethys | 11/12 | 1 | 12/12 | 0 | 11/12 | 6.7% | 7.4% |
| **fabricatorium** | **4/12** | **7** | **11/12** | **0** | **9/12** | 7.5% | 14.0% |
| jinzhou | 10/12 | 1 | 11/12 | 0 | 9/12 | 8.9% | 11.6% |
| timeriftruins | 11/12 | 0 | 12/12 | 0 | 10/12 | 9.1% | 13.2% |
| avinoleum | 10/12 | 2 | 12/12 | 0 | 11/12 | 9.3% | 10.1% |
| blackshores | 10/12 | 1 | 11/12 | 0 | 11/12 | 9.8% | 10.9% |
| laguna | 11/12 | 1 | 11/12 | 0 | 9/12 | 12.1% | 14.3% |
| roysurface | 9/12 | 2 | **7/12** | **3** | 8/12 | 13.2% | 23.9% |
| mengzhou | 12/12 | 0 | 12/12 | 0 | 12/12 | 17.6% | 23.6% |
| mengshutianluo | 11/12 | 1 | 11/12 | 0 | 11/12 | 20.2% | 25.0% |
| qiqiu | 11/12 | 1 | 11/12 | 1 | 12/12 | 20.6% | 24.2% |
| darkplain | 9/12 | 2 | 12/12 | 0 | 11/12 | 23.2% | 35.2% |
| lowervault | 10/12 | 1 | 11/12 | 0 | 10/12 | 24.2% | 32.8% |
| lahai | 10/12 | 2 | 11/12 | 1 | 12/12 | 27.8% | 36.8% |
| **合计** | **139/168 (82.7%)** | **22** | **155/168 (92.3%)** | **5** | **146/168 (86.9%)** | **13.2%** | **17.7%** |

重跑原门禁逐包等于 10-08 的数（`sweep-art-recheck` 与 `sweep-sift-per-tile` 完全一致）。

**两条互相印证的结论：**

1. **82.7% 这个数混了两件事。** 168 道题里有 **22 道任何检测器都答不出**。剩下 **146 道里过了 139 道
   = 95.2%**。换成纹理门禁只抽到 5 道无答案的题，剩下 163 道过 155 道 = **95.1%**——**两个门禁算出同一个
   有条件通过率**，这是内部一致性检验。
2. **fabricatorium 是「12 道题里有 7 道无答案」的那个包，14 个包里最高。** 它那 5 道有答案的题过了 4 道。

### 这不是 SIFT 的问题

线上那套 SURF 的关键点坐标与瓦片像素**是 1:1 的**（`scale = 1.205` 与 `virtualMapSize = 850` 在
`KuroTilePointToAppMap` 里正好抵消），所以能直接换算回拼图像素比对（校验：32194 个点 100% 落在图内，
其处 `|Laplacian|` 是全图均值的 **10.0 倍**）：

- 同样那 12 个框里，**有 6 个连 SURF 也不足 8 个点** → 换成 SURF 一样必然失败；只有 2 个框 SURF 有可能翻盘；
- 全 14 包 31,426 个查询尺寸窗口：**SIFT 有 13.2%、SURF 有 17.7% 能凑够 8 个点**。SURF 的点数是 SIFT 的
  **3.6 倍，覆盖率只从 13.2% 提到 17.7%**。**这是地图与参照集的性质，不是检测器的。**

### 覆盖率低是全局现象，不是隐海独有

**14 个包 31,426 个窗口里，82.3% 的位置对 SURF 都没有答案。** 所以「合成裁剪」这个协议本身就是一个
**弱仪器**：它从「八成位置答不出」的池子里抽样，成绩主要由**抽到几道无答案的题**决定，而不是由检测器
质量决定。它测出来的 82.7%，是**地图有多少内容**和**门禁挑得准不准**两件事的乘积。
**用它比较检测器之前，必须先把它改成有条件通过率。**

### 留下的工具（都在 `measurements/`，⚠️ 该目录在 `.gitignore` 里，不在版本库）

| 位置 | 作用 |
|---|---|
| `accuracy-check/imao_vs_sift.py` | 新增 `--gate art\|texture\|answerable` 与 `--gate-reference`；报告新增 `queries[]` 逐题诊断 |
| `accuracy-check/sweep_per_tile.ps1` | 新增 `-Gate`；汇总新增 `unanswerableQueries` / `referenceKeypointsMin` / `referenceKeypointsMedian` 与覆盖率判读表 |
| `accuracy-check/coverage_windows.py` | 新脚本：逐包量「有多少查询尺寸窗口存在答案」，SIFT 与 SURF 各一份 |
| `sweep-texture-per-tile.json`、`sweep-answerable-per-tile.json`、`sweep-art-recheck-per-tile.json`、`coverage-windows.json` | 本次证据；原 `sweep-sift-per-tile.json` **未覆盖** |

### 遗留（不要忽略）

- ⚠️ **roysurface 在纹理门禁下从 9/12 掉到 7/12，未解释。** 它有 3 道题无答案，比原门禁还多——
  说明 `mean|Laplacian| ≥ 6.0` 对**它**挑得比 `std ≥ 18` 更差。**换门禁不是单调改进**，
  两个门禁各有一个包吃亏。有条件通过率只有 `answerable` 门禁能保证（它抽不到无答案的题），
  代价是它会连边缘题一起抽（`referenceKeypointsMin` 正好是 8），所以那个数最保守。
- ⚠️ **定位精度的 SURF↔SIFT 对照仍然缺**（7.4）。上面只证明了**覆盖面**相当；「同样有答案的两边，
  谁认得准」还没有测量，而 Python 的 OpenCV wheels 创建不了 SURF。
- 顺手修了一个重构引入的缺陷：`reference_for` 的缓存键原先漏了 `with_grid`，会让
  `--config <名>-grid` 悄悄退回未截断的集合。**对本次任何数字都没有影响**——那个 `-grid` 路径在
  出货常量（150 格 × 200）下从不触发（原文第三节③已证），修的是将来的静默错答。
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

### 已发布：v2026.10.10.1（2026-10-10，sequence 44）

`codex/program-update-disk-reuse`、`codex/synthetic-crop-gate-correction`、
`codex/distribution-size-reduction` 三条分支合并后发布。检测器那条是**纯文档**，玩家看不到任何变化，
因此没有进公告。

**发布产物实测（同一个仓库相邻两个 Release，可比）：**

| 产物 | v2026.10.8.1 | **v2026.10.10.1** | 变化 |
|---|---|---|---|
| **`core.zip`** | 27,351,470（26.08 MiB） | **2,072,817（1.98 MiB）** | **−24.11 MiB** |
| `ui.zip` | 61,649,335（58.80 MiB） | 61,890,716（59.02 MiB） | **+0.23 MiB** |
| `runtime.zip` | 150,216,228 | 150,216,216 | −12 B |
| `windows-x64.zip` | 468,567,868（446.86 MiB） | 442,002,065（421.53 MiB） | **−25.33 MiB** |

- **`core` 的 24.11 MiB 与预演算的 1.98 MiB 完全一致**，三方（本地构建 2.03 / 预演 1.98 / 正式发布 1.98）互相对得上。
- **`ui` 反而涨 0.23 MiB** —— 8.2 那条「只省磁盘不省流量」在正式产物上再次成立。压缩带来的 ~63 MB
  是**安装后的磁盘**，不是下载。
- 安装 ZIP 少 25.33 MiB，正好是那 23.77 MB **不可压缩 PNG** 的量级；启动器/桥接在 ZIP 里本来就压得
  差不多，所以不贡献——**与 8.1/8.2 的模型一致**。

**发布链路的完整性：**

```
sequence 44   transaction 046cccedd037459c92772cd29dca3b46
frozen artifact 11673455016   sha256 ce4870b676492f02e7dc39266eb64a919b772334460939cfcc7e1b2d3b9ee801
install zip                   sha256 c1cd33db7a93c482c19527d7bfd612dafa322c86166118fa12fdbbfdccfb2527
```

- 构建 Run `38059112873`（build + prepare，success，46 分 08 秒，含等审批）→ 本地签名 → 发布 Run
  `38062298860`（publish / cleanup / mirrorchyan / gitee **四个 job 全 success**，8 分 44 秒）。
- `release-authorization.json`（14,472 字节）随 Release 发布，第三方可用 `verify-install-authorization`
  独立校验手动安装 ZIP；`release-signing-request.zip`（301,335 字节）永久附在 Release 上供恢复。
- 大 artifact（1.06 GB 的 `release-build`）已被清理，只剩两个小文件。
- ⚠️ **发布流程会自己往 `main` 推提交**（`updates/release-state.json` / `channel-state.json` /
  `stable.json`）：`4f7389a` 预留 sequence → `30498db` 注册请求 → `1264eaf` 接受签名 →
  `aa47d18` 记录 Release ID → `cbd5a8f` 同步 stable。所以发布期间**本地 `main` 必然落后**，
  按 `Docs/CloudRelease.md` 用 `git merge --ff-only origin/main` 追平即可（**不要**用
  `git fetch origin main:main`）。我一开始看到发布 Run 的 `head_sha` 不是自己推的那个 SHA，以为出了问题，
  其实是这个机制——**下次别再误判**。
- ⚠️ 更正一条我先前给人看过的过时流程：仓库里**没有任何地方**读
  `CLOUD_RELEASE_REHEARSAL_RUN_ID`（全库 grep 零命中）。正式发布**不需要**设它：冻结产物在同一次
  Run 内先用临时密钥 finalize，`prepare` 从**同一趟**取工件并核对摘要。别再按旧记忆去建那个变量。

⚠️ **发布后仍然悬着的两条（未关闭）：**

1. **端到端点加载路径至今没在真机验证过。** 见本节上文；`v2026.10.10.1` 的首次玩家启动就是那次验证。
   若出现点位不显示或 `required scene points missing`，第一处要查的就是它。
2. **硬链接的实际节省量仍未实测**（见 4.9）。要等一次**真实更新**后才能与 1,914 MB 的三棵树量值对账。
