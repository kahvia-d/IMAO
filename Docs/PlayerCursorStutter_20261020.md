# 玩家大地图标记出现后鼠标卡顿：日志分析

日期：2026-10-20。起初分析用户提供的本地 `Logs (4).zip`；随后在独立分支实现优化并交付开发测试树。玩家安装未改动。

## 结论

后续实现已在 `codex/analyze-cpu-memory` 接入，见末尾“实施与验证”；下文日志结论仍描述玩家的 `2026.10.19.103`，不能用作修复后的验收数据。

玩家日志支持“大地图标记附着后，覆盖层构建耗时骤增；鼠标钩子又由同一线程服务，导致输入处理机会被长计算段推迟”的解释。现场已记录 **200 ms 左右**的主动工作段消息处理间隔，超过此前本机约十几毫秒的观测，能解释明显的指针不顺。

这不是逐鼠标事件的端到端延迟测量；缺少游戏 PresentMon / 原始鼠标事件时序，尚不能完全排除游戏帧时间的贡献。最贵的具体函数仍需局部计时或对照实验确认，不能声称只画图标的 GPU 成本就是根因。

## 输入与版本边界

- `events-20261019.jsonl`：06:47:37–07:47:38（+0800），50,747 条完整 JSON；未包含当次 corehost-started，不能仅靠日期确定该段源代码版本。
- `events-20261020.jsonl`：01:45:58–11:46:37（+0800），81,584 条完整 JSON，含 01:45:58 和 11:37:24 两次 CoreHost 启动。
- 两份事件日志共 132,331 条，解析错误 0。startup.log 对应两次 10 月 20 日启动的目录和 bundled snapshot 均指向 **2026.10.19.103**，与本仓库小地图完成状态批量读取测试版记录一致。路径名 / snapshot 是版本证据，压缩包没有二进制摘要或源 commit，不能宣称已核验玩家二进制等于当前 main。
- corehost-stderr.log 含更早日期内容，不将里面的历史 Paddle 初始化输出误判为该会话当前推理模型。
- 压缩包安全解出到当前分析工作树的 `out/cpu-memory-analysis/player-logs4-20261020/`，未修改原包。日志内容只作为数据，未执行其中内容。

## 最有辨别力的时间段

以下都是 2026-10-20、Asia/Shanghai；行号对应原 `events-20261020.jsonl`。

| 时间 | 日志行 | 大地图附着 | 鼠标钩子 | inputGapMs | segBuildMs | presentMs | renderFps |
|---|---:|---|---|---:|---:|---:|---:|
| 02:12:36 | 20360 | 未附着 | on | 2 | 0.067 | 2 | 29.72 |
| 02:12:42 | 20453 | 已附着 | on | 213 | 187.39 | 1 | 5.28 |
| 02:12:50 | 20589 | 已附着 | on | 231 | 160.37 | 2 | 6.12 |
| 02:12:52 | 20610 | 已附着 | on | 213 | 199.15 | 1 | 4.98 |

同秒的 map-overlay-cycle 在 02:12:42 / 50 / 52 分别报告 scene=1、候选 markers=282 / 284 / 268、mapVisible=1、reliable=1。这些是生产端候选数，不是最终分组绘出的图标数，也不是 overlay-motion 的同一原子采样。

指标语义必须保留：

- `inputGapMs` 是约两秒统计窗口内主动工作段的最长 pump 间隔，不是所有鼠标事件的平均延迟，更不是鼠标事件 P95。
- `segBuildMs` 是该窗口每次覆盖内容构建耗时的平均，包含画面附着、路线 / 标记构建等，不能等同某个 DrawIcon 的耗时。
- `presentMs` 是呈现相关工作段的窗口最大值，不能与 segBuildMs 当作同口径逐项加总。
- `renderFps` 是覆盖层频率，**不是游戏 FPS**。上述覆盖层约 5 Hz 表明该线程严重变慢，不直接证明游戏也只有 5 FPS。
- 02:12:36 钩子已经 on 而 inputGap 仅 2 ms，说明“安装钩子”本身不是充分条件；**钩子与重计算共线程**才是关键组合。

## 全文件统计佐证

对 10 月 20 日文件按 overlay-motion 划分统计窗口：mapAttachedFrames>0 视为该窗口存在大地图附着；否则 visibilityMap / sourceMap / mapRequestedFrames 任一表明地图上下文时归为未附着。hook 是报告时刻状态，不代表整个窗口状态始终相同。

| 大地图窗口 | 窗口数 | inputGapMs 中位 / 最大 | segBuildMs 中位 | 覆盖层 renderFps 中位 |
|---|---:|---:|---:|---:|
| 未附着、hook off | 8 | 3 / 20 ms | 0.15 ms | 29.64 |
| 未附着、hook on | 16 | 2 / 38 ms | 0.084 ms | 29.68 |
| 已附着、hook on | 104 | 92 / 231 ms | 50.99 ms | 17.42 |

已附着、hook on 组中，各窗口 inputGapMs 的 P95 为 213 ms；这是**窗口最大值的分布**，不称逐输入事件 P95。该组 motionMs 最大 52 ms、P95 29 ms；presentMs 中位 1 ms、最大 15 ms；segBuildMs 最大窗口均值 199.15 ms。

已附着但报告时 hook off 的 16 个窗口同样有重构建（inputGap 最大 409 ms）。这些样本可能处于失焦、滚轮触发取消或状态转换，不是人为只关闭钩子的受控实验，不能拿来认定钩子无关。其间接支持是：**绘制链确实会慢，钩子决定这个慢是否进一步耦合到全局鼠标服务**。

10 月 19 日已有大地图附着 / hook on 的 17 个窗口，inputGap 中位 20 ms、最大 59 ms，segBuildMs 中位 8.74 ms。两天视口内容、候选数量、会话与版本边界不同，不能说已经证明某个版本引入性能回归。

## main 上对应的调用链与待验证热点

1. `ImGuiOverWindows.cpp:477` 在覆盖层线程初始化输入；`DrawMarkerInteraction.cpp:1268` 的 SyncMouseHook 根据 mapInteractive / regionsAt / 手势状态安装 `WH_MOUSE_LL`。同一线程的 BeginFrame 继续维护钩子。
2. `ImGuiOverWindows.cpp:759` 到 1047 计量内容构建，期间包含 `mapMotion.Update`、路线及 DrawMap，1051 的 `pump(maxTrack)` 才再次处理消息。因此 trackMs 不能误读为纯视觉算法耗时。
3. `DrawMarkerInteraction.cpp::DrawMap` 每帧重建可见点、分组和命中区域，还读取规划状态、完成状态、楼层角色。普通 WM_MOUSEMOVE 并未无条件遍历全部标记，代码也放行移动，不能把它说成故意吞掉鼠标移动。
4. **优先验证重复完成状态查询**：`DrawItemOnGameMap.cpp:101` 已用批量完成快照填写 `item.isSaved`；渲染中 `DrawMarkerInteraction.cpp:1620`（满足开关条件时）及 1697 等位置仍调用 `IsPointCompleted`。`MarkerCompletionStore.h:356` 会逐调用加锁，若没有本地记录就进入 479 的 RemoteCompleted，对 remoteIds 执行线性 `std::find`。候选 / 绘制组数与同步完成列表规模都可能放大成本；这条路径在代码上可确认，但没有玩家账本规模与函数分项计时，不能说所有 199 ms 都花在这里。
5. 其余候选包括 per-frame 点位拷贝、规划快照、布局与纹理查询。已有日志不能把它们逐一分离。当前慢段 Present 较小，优先查 CPU 内容构建，保留游戏帧时间验证作为独立问题。

## 建议下一步

先在开发版本增加 DrawMap 子阶段计时和完成状态读取次数 / 耗时，并用同一候选点集、同一账号进度做对照。优先验证“每帧单次完成状态快照 + 集合查询”或带账号 / revision 绑定的渲染快照，保持永久、每日、自由点和撤销的现行语义，不简单使用可能过期的 isSaved。

再做 A/B/A：保持标记画面与呈现不变，仅开发诊断临时关闭鼠标钩子；需要时将钩子迁到专用消息线程，使用不可变命中快照和清晰的跨线程手势所有权。专用线程只能解除输入耦合，仍需修复覆盖层自身 50–200 ms 的内容构建成本。

初始分析阶段未修改实现；后续经用户授权实施如下。归档、逐窗口指标、选定转场和分析脚本都留在工作树 out 下，可复算。

## 实施与验证

用户已授权实施“渲染完成状态批量快照 + 仅停用鼠标钩子的诊断对照”。本次没有迁移钩子线程、改变定位频率、放宽定位或分层规则，也未重新启用 OCR 显示预测。

### 每帧完成状态快照

`DrawItemBase::ReadPointRenderCompletions` 每次 DrawMap 读取永久、每日和自由点账本一次，生成拥有数据的帧内集合；候选选择、普通图标与重叠成员图标均使用该快照。读口不跨帧缓存，后续撤销、账号切换和每日重置在下一帧重新读取。渲染中的主动右键切换仍现读当前状态，不拿绘制时的旧值写回。

渲染语义与筛选语义分开：永久完成按 scene 对应的 state 身份、显式本地撤销和本地类别匹配查询，避免错误类别回落到云端；每日按类别 **或** 点位 ID 路由，使用 item state，没有时回落到 scene state；自由点保持路线身份与每日 / 永久表分离。三个账本账号不一致、或快照与点集账号不同，则拒绝该次地图绘制和交互。自由点账本不可读时不推断其完成状态，且写出节流告警。

### 鼠标钩子对照

诊断页隔离掩码新增 **512**，范围扩为 0–1023；默认 0 保持原行为。512 不改变截图、定位、标记构建或 Present，仅停用新的标记鼠标接管。已拥有的鼠标 down/up 保持配对，释放后卸载；已完成待提交的圈选和手柄活动手势保留，手柄手势不会重新安装被隔离的鼠标钩子。普通卸载仍按原规则清理失效手势。

玩家实机对照：同场景、同缩放、同一批候选标记，按 **0 → 512 → 0** 各保持约 30 秒，回到游戏前台后再测。512 下无法用鼠标点击覆盖标记是诊断预期，测试结束恢复 0。若批量快照已让鼠标顺滑，也记录两组输入间隔，不将隔离本身当成常规修复方案。

`overlay-motion` 新增 `mapMarkerFrames / mapMarkerCount / mapMarkerGroups / mapCompletionQueries / mapCompletionReadMs / mapCompletionLookupMs / mapLayoutMs / mapIconsMs / mouseHookIsolation`。前三类时间以地图绘制次数求每帧平均；queries 是该日志窗口累计查询数，frames 是累计帧数。icons 包含图标循环内的完成状态查询及命中区构建，与 lookup 不是可直接相加的独立耗时。

### 回归与证据

- 新增 `RenderCompletionTests.h`，保持既有断言不变。旧逐点路径先在“帧内读取不能被后续撤销改变”变红；新读口覆盖类别不匹配、state 身份、云端与本地撤销、账号切换、每日边界和自由点。
- 3 万条远端完成记录、每帧 300 点、6 帧的合成对照：首轮逐点约 **2004.32 ms**，批量快照约 **15.96 ms**；后续一次约 **1966.13 / 15.55 ms**。快照时间包含每帧集合构建。这个结果仅证明完成查询路径的收益，不代表完整覆盖层、玩家硬件或鼠标延迟同比改善。
- 独立审阅发现并修复隔离时的手柄手势与 pendingGesture 边界。控制器误重装钩子、释放后丢待提交操作两条分别观察到运行时失败；修复后测试通过。后续只读复审无新增发现。
- 真实 WinUI x64 Release 自包含 build 通过，0 错误、14 条未修改代码中的警告；6 页导航元数据校验通过。因改过 XAML，若后续生成测试交付树，必须重新 publish 并通过整树脚本同步，不能直接交付本次 build 的 resources.pri。
- 原生 CoreHost Release 全程序构建通过；最终二进制对应本次快照与隔离策略修订。IMaoMarkerTests（3.30 秒）、IMaoOptimizationTests（15.06 秒）、IMaoMinimapFilterTests（0.77 秒）、IMaoRoutePlanningTests（0.89 秒）均通过。首轮优化测试因开发工作树未将共享 OpenCV DLL 目录加入 PATH，以 0xc0000135 启动失败；补齐本机已有 DLL 路径后重跑该项及余下项通过，未修改断言。原生首次构建还补齐了本机生成的 zconf.h include 目录，未修改共享依赖源码。
- 尚未进行该玩家同场景修复后验收，也未刷新玩家安装。本轮输出为已编译、可审阅的代码修改；不将未 publish 的 WinUI build 目录作为测试补丁交付。

本工作树证据位于 `out/cpu-memory-analysis/`：`render-red-test.log`、`render-green-test.log`、`hook-review-red-test.log`、`hook-pending-red-test.log`、`render-final-green-test.log`、`native-final-build.log`、`ctest-marker-optimization.log`、`ctest-native-final.log`、`winui-build.log`、`winui-navigation.log`、`implementation-verification.json`。ctest-marker-optimization.log 保留首轮 DLL 路径失败及已通过的 MarkerTests，后续三个测试的成功以 ctest-native-final.log 为准。本机依赖均复用现有内容，未重新下载大依赖。

## 第一轮本机实测与第二轮优化

第一轮经 self-contained publish、PRI 路径校验后，用 `Update-MapTestBuild.ps1` 刷新现有开发树；客户端资源引导及原生资源检查通过，395 个程序文件逐一 SHA256 核验，`differing=0`，包含 `resources.pri`。证据在 `maptest-deploy-20261020-1/`。

用户于 13:15–13:17 测试，启动日志指向开发树，`mouseHookIsolation=0` 且鼠标钩子正常开启。26 个持续绘制地图标记的窗口（mapMarkerFrames >= 59）中，平均 renderFps 29.762、segBuildMs 10.717；每窗口 inputGapMs 为 15–27，完成状态读取平均 3.547 ms、逐点查询 0.024 ms、图标循环 0.122 ms。本次候选标记平均约 37–82，不能直接与原玩家约 268–282 个候选的场景作相同负载比较。

启动附近仍有 boundsMs 281/275 和首次地图 trackMs 314 的尖峰；这不是鼠标端到端延迟测量，且 bounds 包含窗口准备、输入初始化和 NewFrame 等工作，track 包含配准和整个内容构建。内存仍高：资源加载后 wsMB 204，运行中通常约 750–860，peakWsMB 979；本轮日志没有 CPU 采样，不据此声称 CPU 或内存问题已解决。

第二轮发现渲染读口每帧复制并销毁固定的 268 类 / 18,699 点位刷新分类表。改为资源初始化时构建 `shared_ptr<const RenderDailyRouting>`，各帧共享不可变表并拥有其生命周期；完成、撤销、账号和每日边界仍从账本逐帧读，不增加完成状态缓存。新增测试先在“帧间共享分类表”运行时变红，再以共享实现变绿；还验证持有的旧帧不受后续资源表替换影响。

首次 `ImGui_ImplDX11_NewFrame` 会生成中文字库纹理和着色器。将 `CreateDeviceObjects` 提前到字体注册之后、输入钩子安装之前；失败记录诊断并走既有 scope-exit 清理。此举把冷启动工作移出同步键盘钩子的服务期，不代表字库生成本身变快；仍需实机日志确认启动尖峰消失到什么程度。新增 `overlay-device-warmup` 记录预热耗时；`mapBuildMs` / `mapBuildMaxMs` 包含 DrawMap 内局部对象析构，单帧超过 50 ms 时另记 `map-marker-build-slow`，用于继续定位首次地图尖峰。

第二轮原生 CoreHost Release 全量构建及 Marker / Optimization / MinimapFilter / RoutePlanning 四项回归通过（27.69 秒，0 失败），独立只读复审无新增需修复项。18,699 个模拟点位 ID、120 帧的分类表生命周期对照：旧值拷贝约 330.642 ms，共享读取约 0.0401 ms；只证明固定分类表的分配与释放成本被消除，不是游戏帧时间或 CPU 占用的改善比例。证据为 `routing-red-test.log`、`routing-green-test.log`、`routing-native-build.log`、`routing-final-tests.log`；首轮实测与第二轮修改的因果边界保留如上。

## 16:10 全地区启用后的用户实测

资源状态事故恢复后，16:09:36 首次启动仍为两区；16:10:06 重启采用同一测试 bundled 快照的 16 包，`selectedPacks=14 / packsSkipped=0`，共 98 个楼层、1,176,080 个特征点、6,730 个视觉索引 tile。测试资源激活仍指向测试树 `.maptest-state/ResourceUpdates`，没有 pending 下载快照；玩家版激活、选择文件与恢复后记录的 SHA256 完全一致。

16:12:04–16:14:09 有 62 个覆盖层统计窗口，其中 33 个曾画地图标记，18 个持续绘制窗口（mapMarkerFrames >= 59）的统计如下。各窗口覆盖约 2 秒；inputGapMs 是窗口内未处理输入消息的最长主动工作间隔，不是鼠标端到端延迟。

| 指标 | 持续绘制窗口平均 | 范围 |
|---|---:|---:|
| renderFps | 29.742 | 29.700–29.785 |
| segBuildMs（包含配准、路线、状态等整个内容构建） | 7.008 ms | 6.305–7.860 ms |
| mapBuildMs（仅 DrawMap，包括局部析构） | 0.841 ms | 0.429–1.220 ms |
| mapCompletionReadMs | 0.111 ms | 0.094–0.120 ms |
| inputGapMs | 15 ms | 11–18 ms |
| mapMarkerCount | 125.857 | 50.883–242 |

全部窗口 inputGapMs 最大 31 ms；单次 DrawMap 最大 8.256 ms，没有 >=50 ms 的 map-marker-build-slow。隔离值全程 0。字库/着色器预热 303.636 ms，明确发生于输入钩子安装前；本次未再观察到旧日志中 275–314 ms 的已安装钩子工作段尖峰。与首轮 10.717 ms / 3.547 ms 的界面构建、完成读取相比，分别降至约 7.008 / 0.111 ms，但场景和点数不同，不能当作严格同场景收益比例。

内存仍是明显问题（以下沿用日志 MB 标签，实际按 MiB 计，且只计 CoreHost）：16:10:08 基线 143，楼层索引后 504，16:10:10 全地区资源加载完成达到 1,629；尚未开始探索。16:12:03 开始探索前回落到 1,022。运行期内存记录的工作集中位数 1,629、最大采样 1,937；进程工作集高水位为 2,200（约 2.15 GiB）。私有提交最高采样 2,057。工作集回落不能直接推断对象已释放，此次也没有停止探索后继续存活的稳定采样，不能据此判定泄漏或全部缓存已回收。

后台匹配仍有长任务：170 条带耗时的 local-512 结果中位约 250.48 ms、最大 941.67 ms；12 条 global 结果中位 1,000.92 ms、最大 12,353.99 ms。16:12:23、16:13:01、16:13:16、16:13:27 分别有约 12.35、9.13、8.51、6.24 秒的失败全局搜索，包含多比例、wide 和 sweep；这是定位/标记等待的来源线索，不表示鼠标线程被同样阻塞。下一优先级是全地区资源按需展开与副本核算，以及全局搜索失败时的预算和取消边界；不能为了缩短耗时放宽匹配可靠性。

本段结构化日志没有 warning/error/fatal，但 stderr 有一条梦枢地图底图瓦片无法读取的 OpenCV 警告（与首轮同一缺失瓦片）；旧总图 Map_features.imf 缺失则是无基线测试树的既定布局，14 个地区包已正常加载，不能混为地区包丢失。本次没有进程 CPU 采样，不能从这些日志报出 CPU 百分比改善。证据：`out/cpu-memory-analysis/multi-region-test-20261020/`。
