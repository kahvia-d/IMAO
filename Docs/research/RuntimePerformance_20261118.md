# IMao 探索与地图识别性能调查（2026-11-18）

分析基线为远端 `main` 的 `8ef8d422`，程序版本 `2026.11.11.2`。建议先消除探索采集中的重复整帧复制，再优化大地图首次定位和失败搜索。小地图应分别看稳定跟踪、坐标 OCR 和丢失后的恢复。三条路径的耗时相差很大，统一降频会把等待加到原本已经很快的路径上。

## 当前分支的回放结果

在 i5-12600KF（10 核、16 逻辑处理器）上，用当前源码重新构建 Release 版 `IMaoVisualRegression`。用相同目标文件链接诊断入口，与原回放器放在同一目录，使用同一 OpenCV DLL、仓库 Assets 和既有清单。四个新进程串行运行，对照组用默认值，实验组在进入回放前调用 `cv::setNumThreads(2)`。每轮均在进程内读回 OpenCV 设置。每轮都是同一张大地图的六种正例和两个负例，样例间沿用定位器缓存。下表的 CPU 时间含资源初始化和整轮回放，是进程内核态与用户态耗时之和。[回放清单](../../Tests/VisualLocalization/20260906-viewport.json)

| 顺序与配置 | 通过 | 六个正例耗时中位数 | 噪声拒绝耗时 | 整轮 CPU 时间 | 进程峰值工作集 |
|---|---:|---:|---:|---:|---:|
| 1 默认（读回 16） | 8/8 | 1,011 ms | 2.84 s | 17.22 s | 459.27 MiB |
| 2 限制（读回 2） | 8/8 | 1,044 ms | 2.97 s | 14.83 s | 459.45 MiB |
| 3 限制（读回 2） | 8/8 | 1,217 ms | 3.04 s | 15.56 s | 459.44 MiB |
| 4 默认（读回 16） | 8/8 | 1,074 ms | 3.00 s | 17.89 s | 459.28 MiB |

双线程两轮平均 CPU 时间少约 13%，正例耗时没有一致优势，峰值工作集几乎相同。四轮噪声均完成同一档的一个双场景组，预算仍受墙钟时间影响。四轮峰值私有提交约 453 MiB。离线进程只覆盖视觉回放，游戏帧率、完整探索内存和玩家端提速仍须实测。

初次实验只设 `OPENCV_FOR_THREADS_NUM=2`，在本机构建的 Windows Concurrency 后端独立探测时，默认和设置环境变量后都读回 16。初次四轮单列为默认配置的波动记录。另一次显式设线程的试探从不同程序目录启动，内存峰值与噪声的候选场景数也不同，上表只采用与原程序同目录的四轮。[OpenCV 环境变量说明](https://docs.opencv.org/4.x/d6/dea/tutorial_env_reference.html)

小地图跨区清单原样跑了两次，各 12 项，均有 1 项未通过。每轮 8 次成功局部跟踪，匹配段中位数分别为 6.15 和 5.79 ms，计时从特征准备之后开始。噪声负例两次均拒绝，跨 8 次尝试合计约 1.8 s。首图在首批候选即取得几何支持、通过位置一致性检查，第二张独立截图才发布。首图要求 `mustContinueSearch`，本次最大已尝试偏移为 0，因此该项失败。噪声样例实际推进到偏移 84。样例以历史一致性为参照，不能计算独立真值准确率。[跨区清单](../../Tests/VisualLocalization/20260907-cross-area.json) · [回放判定](../../IMao-Core/tools/VisualRegression/main.cpp)

建议把两种验收分别落到合适的正例上。这张首图核对位置、几何支持与跨帧发布时机。另设一个第一页无解、后续页才有已知正确候选的固定输入，核对翻页后找到候选并在下一独立帧确认。噪声样例只能证明失败时继续遍历，已有覆盖器单测可验证遍历顺序。应先取得新的正例覆盖，再讨论调整原断言。本轮保留既有测试与两次失败结果。[遍历覆盖测试](../../IMao-Core/tests/CoarseSearchCoverageTests.h)

## 优先优化的路径

### 1. 探索采集先判重，再减少整帧搬运

本地游戏进入稳定探索后，WGC 回读五个 HUD 区域。回调仍把像素散写到完整 BGRA 缓冲，每个 ROI 帧清零未覆盖区域。常规 getter 持锁对完整 Mat 执行 `copyTo`，返回后采集循环才按源序号去重。旧电脑 107 版的一份本地日志中，531 个采集统计窗口有 500 个出现未发布序号，合计 3,018 次。这个计数支持先测判重收益，当前版本和不同机器的重复率要重采。[回读与清零](../../IMao-Core/src/WindowsCapture/WindowsGraphicsCapture/SimpleCapture.cpp) · [常规 getter](../../IMao-Core/src/WindowsCapture/WindowsGraphicsCapture/SimpleCapture.h) · [发布与去重](../../IMao-Core/src/App/App.cpp)

第一步可让 getter 在同一把锁内核对源序号，再决定是否复制，并区分“没有新帧”和“采集无效”。前者维持现有快照及原采集时间，让正常的新鲜度规则继续生效。几何变化和首帧仍须独立处理。以 2560×1440 为例，一张 BGRA 图为 14.06 MiB，每秒 30 次完整复制涉及约 422 MiB 的有效载荷，另有读写及清零成本。这是尺寸推算，实际收益取决于重复率和内存带宽。

第二步可先在完整 Mat 表示下试验按缓冲区记录 ROI 布局。每块缓冲首次用作 ROI、从全帧模式返回、尺寸或布局变化时清空。布局稳定时仅刷新五块区域。若整帧交付仍主导，再让探索消费者直接持有不可变的紧凑 HUD 像素及客户区坐标。大地图、浏览器云游戏、切换中的首帧和录制分别核验。特别要保留诊断回放的像素来源、录制语义与生命周期。对照同帧五块 ROI 逐字节内容、区域外空白、几何代次、窗口缩放与开关地图，再测锁内时间、每秒复制字节、捕获到发布延迟和游戏帧时间。

### 2. 大地图先减少排名和候选准备，再缩短取消尾部

多场景全局请求先按 1、1.5、2、0.75 四档提取中心区域特征并排名，再建搜索计划、比较场景组。每个 region/factor 在同一请求内已有缓存。两秒预算在一个完整组结束时检查。最初的四档排名、候选准备及组内一次耗时调用都可能占用相当时间。本次最终对照中噪声四轮均被拒绝，最长 3.04 s。[全局排名与匹配](../../IMao-Core/src/App/MapViewportLocalizer.cpp) · [预算检查](../../IMao-Core/src/App/MapViewportWorkBudget.h)

优先做能逐项比对的工作。一遍扫描把瓦片分到各场景，保持分数与瓦片号的原有排序。为同一排名复用去重行的临时存储和候选前缀，避免每次 `SliceCandidates` 都按地图全部关键点重新申请并清零计数数组。分别计时特征提取、排名、候选构造、FLANN 建索引和几何验证。若前段仍主导，再试分段准备与允许取消的工作单元。四档排名会影响计划次序，提前接受某一档是识别语义变更，需要跨缩放、跨场景歧义和快拖后停稳的独立对照。每次几何接受仍须完成同组竞争场景验证。

### 3. 小地图优先降低读数尾延迟，给视觉恢复留出执行时间

活跃读数的提交节拍为 100 ms，常规局部跟踪已很短。现版 OCR 至少处理原图、增强图两次检测，疑难裁框还可能试三组重采样。历史 477 帧按桌面节拍回放，去掉首帧后 CPU / DirectML 识别 P95 为 86.7 / 75.2 ms，分别有 15 / 6 帧超过一个周期。当模型本身已经用掉大半周期，单看每秒提交多少次容易漏掉等待。[读数调度](../../IMao-Core/src/Runtime/ReadoutWorkSchedule.h) · [OCR 实现](../../IMao-Core/src/Coordinate/IdentifyWorldCoordinates/YoloCoordinateRecognizer.cpp) · [成组回放](CoordinateYoloCropRobustness_zh-Hans.md)

先把采集、排队、字形拟合、各视图推理、结果被消费和首次显示串成同一帧时间线。首个实现候选是复用 OCR worker 的固定形状输入张量与图像缓冲，并缓存按字高缩放的静态字形模板。逐帧对照 XY、Z、负号和拒识，记录每帧实际推理次数。追踪正常、无坐标、遮挡和换区恢复分开计时。较大改动是协调 OCR、读数视觉探针、楼层分类和全局恢复的重活开工时机，已完成读数优先消费，恢复搜索保留最低执行机会和遍历进度。Direct 显示、独立帧确认、位置时效与楼层证据沿用现有规则。[行定位与模板](../../IMao-Core/src/Coordinate/IdentifyWorldCoordinates/CoordinateRowAlignment.cpp) · [异步视觉探针](../../IMao-Core/src/App/ReadoutVisualWorker.cpp)

### 4. 把启动峰值、活动常驻和匹配临时内存分别治理

当前已保留原编码、映射描述子、紧凑存储索引并延迟加载旧 OCR。历史全地区探索的一次 CoreHost 现场记录中，周期工作集中位 660 MiB、进程工作集峰值 910 MiB。它与本次离线回放的 458 MiB 属于不同负载，不能相减估算模块占用。[现场内存账本](../plans/RuntimeMemoryPlan_20261020.md)

启动可试按清单预先确定行号范围，逐包解码写入最终存储后及时释放来源。现行加载先拿到一批包，再统一合并和映射。新实现须逐字节校对描述子与索引引用、兼容旧格式，并在整份资源就绪后一次发布。活动期则优先量化采集帧所有者和缓存构造峰值。大地图配置 48 MiB / 16 项是留存限额，候选与 matcher 会先构造再决定淘汰或临时使用。小地图 64 MiB 是按 float 等价字节计费。两者直接相加无法得到实际常驻。记录当前留存、在建、临时条目、命中和重建耗时，再试预留空间或延后构建。用同一路线反复跨区检查是否以重复训练或硬缺页换取账面下降。[资源加载与合并](../../IMao-Core/src/Feature/RuntimeFeatureRepository.cpp) · [大地图缓存](../../IMao-Core/src/App/MapViewportLocalizer.cpp) · [小地图缓存](../../IMao-Core/src/Feature/Match/FeatureRowCache.h)

## 如何判定对玩家有效

2026-11-19 在分析分支实现了第一步。WGC 采集循环在帧锁内按源序号复用最近一次读取的独立像素。重复读取仍经过客户区裁切、录制与发布判断，保留源采集时间。窗口失效、几何代次变化和无效画面会释放缓存。每个 `capture-cadence` 统计窗口新增 `wgcCopiesAvoided` 和 `wgcBytesAvoided`，可用于现场对照。离线以 100 张 2560×1440 BGRA 帧各读两次，200 次输出与逐次复制的像素一致，共跳过 100 次复制（1,474,560,000 字节）。重复率取自合成输入，游戏帧时间收益待实测。

建议第一轮只改采集判重，独立比较旧、新路径。固定游戏帧帽、分辨率、路线和地区选择，轮换执行工具关闭、当前默认、候选、当前默认。分别覆盖正常探索、跨区失锁恢复、大地图冷开和快拖后停稳。记录游戏帧时间 P95/P99、超过目标帧预算的比例、呈现模式，并同步记录 CoreHost CPU 时间、线程就绪等待、GPU 忙碌及采集读回。这样能区分 CPU 争用、覆盖层合成和 GPU 读回。离线回放无法单独归因游戏掉帧。[帧时间采集](../../scripts/Measure-RealOverlayTrace.ps1) · [分项隔离实验](../../scripts/Measure-WorkIsolation.ps1)

同轮记录首次可信定位与采集到显示的 P50/P95、错误发布与拒绝、各进程的 private bytes 与工作集，以及切场景前后的峰值。小地图稳定跟踪、大地图首次显示、失败搜索分别报告，避免平均值掩盖一条路径的退化。优先在旧电脑与日常机器重复。只有游戏帧时间改善、识别正确性与恢复能力能对账，才值得调整生产默认值。

采集和几个识别 worker 已调用 Windows 的低于普通优先级设置，CPU OCR 的 ORT 会话配置为算子内 4 线程、算子间 1 线程且已关闭算子内自旋。OpenCV 的并行区另有自己的配置。试验时先锁定 OCR 后端，分别改变 OpenCV 与 ORT 的并发，按相同工作量比较 CPU 时间及游戏线程就绪等待。DirectML 单列并记录 GPU 帧时间，特别是在游戏本已受 GPU 限制时。[线程优先级](../../IMao-Core/src/Runtime/ThreadPriority.h) · [ORT 会话配置](../../IMao-Core/src/Coordinate/IdentifyWorldCoordinates/YoloCoordinateRecognizer.cpp) · [ORT 线程模型](https://onnxruntime.ai/docs/performance/tune-performance/threading.html)

## 复查本轮数据

在仓库根目录先执行 `. .\scripts\Enter-DevEnvironment.ps1`，以现有 CMake 配置构建 Release 目标 `IMaoVisualRegression`。运行 `x64\Release\IMaoVisualRegression.exe <仓库根> <清单路径> <报告路径>`。大地图清单是 `Tests/VisualLocalization/20260906-viewport.json`，小地图是 `Tests/VisualLocalization/20260907-cross-area.json`。本机原始报告在 `out/perf-study-20261118/`。表中数据见 `peer-summary.json` 和四份 `peer-*.log`，诊断入口与计量代码见 `thread-probe.cpp`、`run-peer.py`。重跑线程对照时将该目录下的 `ThreadProbe.exe` 临时复制到原回放器旁并命名为 `IMaoPerfStudyProbe20261118.exe`，再从仓库根运行 `run-peer.py`。采集重复计数来自 `out/oldpc-logs6-107/events-20261101.jsonl` 中各次 `capture-cadence` 的 `captureUnpublished` 之和。
