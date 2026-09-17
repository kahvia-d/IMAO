# 2026-09-17 游戏掉帧：工具侧开销分析与改动

## 现象

使用工具时玩家感觉游戏"有点掉帧"，但说不清是工具还是机器上的其它软件。

## 实测环境（这台机器）

| 项目 | 实测值 |
| --- | --- |
| CPU / GPU | 10 核 16 线程 / RTX 4060 |
| 显示器 | 2560x1440 @ 165 Hz（Windows 缩放 125%） |
| 游戏 | `Client-Win64-Shipping.exe`（鸣潮），2560x1440 无边框全屏，进程受保护（取不到 `Path`） |
| 同机常驻的叠加/串流类软件 | GameViewer 全家桶（含虚拟显示器适配器，常驻未使用）、`Gamebar_Connect`、`GameBarPresenceWriter`、`MSI_GamebarTool`、`XboxPcAppFT`、4× `nvcontainer` |
| 工具未开、游戏非前台时 | 游戏 ~1 核 CPU、28% GPU |

> 注意：以非 DPI 感知的 PowerShell 查询会看到 2048x1152，那是 125% 缩放后的虚拟尺寸，不是真实分辨率。

## 工具自身开销（来自日志与代码，不是估计）

- 覆盖层窗口是**整个游戏画面大小**（2560x1440）的置顶分层窗口（`WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT`），并且**每秒无条件 `Present` 30 次**：`overlay-motion` 的 60 个采样全部是 `renderFps≈29.8`，哪怕画面内容完全没变、甚至全透明。合成量约 110 Mpx/s，而游戏自身 165 fps × 3.69 Mpx = 609 Mpx/s。
- WGC 采集：`readbackAvgMs≈26 ms`/帧（阻塞在帧池线程，不在游戏线程），到达率被它限到 ~33 fps；消费侧 `captureAvgMs=4.4`、`captureMaxMs=5.1`。
- BitBlt 采集：`captureAvgMs=30.9 ms`（最长 138 ms），32 秒内 3 次 >50 ms。这是 `PrintWindow` 拉取整帧窗口内容的同步成本，而且它**作用在游戏窗口的呈现路径上**。
- CPU：OpenCV 是**单线程**构建（`WITH_TBB/WITH_OPENMP/WITH_OPENCL` 全 OFF），OCR 明确限成 `clamp(核数/2,2,4)=4` 线程且这几次会话里没跑；识别循环 9-11 Hz、每次 ~1 ms；`map-viewport` 突发 ~100 ms。
- 之前**没有任何优先级调整**：工具进程与游戏同为 Normal，约 12 个线程。

## 结论：机制与确定性

1. **覆盖层窗口把游戏的呈现路径从独立翻转压回 DWM 合成**（机制明确、可测、影响最大）：无边框游戏本来可能直出/走 MPO，被一个全屏置顶分层窗口压住后必须走合成；再叠加每秒 30 次的无谓重合成，在 165 Hz 上表现为帧节奏抖动。玩家说的"掉帧"经常是这种 judder，而不是帧率数字下降。
2. **BitBlt 模式的 31 ms 全屏同步读取**：默认值本来是它，属于"确实会打到游戏身上"的成本。
3. **其它常驻叠加层**（Game Bar、MSI 游戏栏、GameViewer 虚拟显示器）会做和 1 同样的事，是"或者其它东西"的现实候选。
4. CPU 争用：量级不大（约 1 核上限），但工具没降优先级，属于顺手能削掉的部分。

## 本次改动

- **A. Windows Graphics Capture 成为默认**（`IMao-WinUI/Models/RuntimeConfiguration.cs` 默认值 1、`DLL_API.cpp` 的 `CaptureWay` 默认 1、旧配置按 `ConfigVersion<2` 迁移一次），并且 **WGC 出不了首帧时自动回退 BitBlt**（`DLL_API.cpp` 的 `RunOverlayAttempt` 重试一次，诊断记 `capture-fallback`/`capture-attempt-failed`，玩家侧只看到一条提示）。为此去掉了 WGC 建项失败时的 `MessageBoxW`——它会在回退发生的时刻阻塞进程，而且弹在游戏后面没人能点。
- **B. 覆盖层只在内容变化时呈现**（`OverlayPacing::ShouldPresentFrame` + `HashOverlayDrawData`）：内容不变的帧不再重绘、不再 `Present`，DWM 继续显示上一张表面；完全没有内容超过 1 秒（`OverlayPacing::ShouldHideIdleOverlay`）时直接 `ShowWindow(SW_HIDE)`，让合成器彻底忽略这个窗口。诊断 `overlay-motion` 新增 `presentSkipped=` 与 `windowHidden=`。
- **C. 重活线程降优先级**（`Runtime/ThreadPriority.h`）：采集、游戏状态检测、OCR/坐标搜索、大地图定位、视觉定位、资源预载线程都设为 `BELOW_NORMAL`；**覆盖层渲染线程保持 Normal**，因为它同时服务输入钩子——钩子回调等 CPU 就是玩家能感觉到的延迟。

## 实测（PresentMon 2.5.1，逐帧 `MsBetweenPresents`，游戏 `Client-Win64-Shipping.exe`）

工具：`out\perf\PresentMon-2.5.1.exe`（NVIDIA FrameView SDK 自带的 1.x 构架在提权下也静默返回失败，不能用）。原始数据：`out\perf\frames-run1-bitblt.csv`、`out\perf\frames.csv`。PresentMon 的 `--date_time` 列比本地时间快 8 小时，按此对齐工具日志。**它需要提权**（脚本 `out\perf\trace.cmd`，`--terminate_after_timed` 必须带，否则进程不退出、下一轮无法建会话）。

**第一轮：工具关 → 工具开（BitBlt）→ 工具关**

| 相位 | 时长 | fps | 帧时间 中位 / p99 / 最大 | >33ms | >50ms | >100ms | PresentMode |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 关 | 135s | 59.7 | 16.66 / 18.95 / 39.7 | 3 | 0 | 0 | Composed 100% |
| **开（BitBlt）** | 123s | 74.7 | 12.79 / **23.86** / **161.0** | **28 (0.30%)** | **13 (0.14%)** | **3** | Composed 100% |
| 关 | 134s | 77.9 | 12.10 / 19.41 / 76.2 | 11 (0.11%) | 3 (0.03%) | 0 | Independent Flip 80% |

- 三个 >100 ms 的游戏巨帧与工具 `captureMaxMs` 尖峰同秒一一对应（157.5↔171.6、161.0↔166.2、101.2↔100.8 ms），机制明确：`captureAvgMs` 中位 32.8 ms 而采集周期 33.3 ms，采集线程占用约 **97%**，103 秒内 39 次慢采集。**BitBlt 是主犯。**
- 该轮游戏的 `SyncInterval`/`ALLOW_TEARING` 在中途自行改变（60 → 75-78 fps），跨相位只能按同步设置分段比较。

**第二轮：工具关 → 工具开（WGC）→ 工具关**（游戏全程前台，日志 `focused=1`）

| 相位 | 时长 | fps | 帧时间 中位 / p99 / 最大 | >20ms | >33ms | >100ms | PresentMode |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 关 | 151s | 72.2 | 15.11 / 20.66 / 108.2 | 1.07% | 0.24% | 1 | 混合（IF 占 40%） |
| **开（WGC）** | 120s | 77.5 | 12.36 / **25.10** / 112.7 | **3.73%** | 0.38% | 1 | **Composed 100%** |
| 关 | 380s | 67.5 | 16.15 / 19.32 / 141.6 | 0.78% | 0.13% | 1 | 混合（IF 占 32%） |

- **巨帧消失**：工具侧的 `captureAvgMs` 从 32.8 ms 降到中位 5.3 ms（占空比 97% → ~16%），游戏再没有与采集相关的 >100 ms 帧，最大帧时间回到与"工具关"相同量级。
- **仍有小幅剩余代价**：>20 ms 的帧从约 1% 升到 3.7%；20 秒滑动窗口显示工具开着的 6 个窗口稳定在 2.6-6.5%，相邻"关"窗口 0.8-1.3%（但工具关着时也出现过 4.45% 的窗口，说明一部分是游戏自身的）。
- **这份剩余代价不是我们的阻塞调用**：82 帧游戏 >25 ms 时同期 `captureMaxMs` <15 ms；`boundsMs` 只在启动瞬间出现过一次 270 ms（`SetWindowPos`）。
- **剩余机制**：工具开着时游戏 100% 停留在 `Composed: Flip`，工具关掉后有四成帧能走 `Hardware: Independent Flip`——覆盖层窗口在场就把游戏压回合成。另有 WGC 自身的持续流量（`readbackAvgMs≈24 ms`/帧、约 33 fps，以及每帧 14 MB 的 GPU 拷贝与 `cv::Mat` 拷贝）。
- **本轮改动在实机生效的证据**：`overlay-motion` 报 `presentSkipped` 116 秒内 1138 次（呈现从 30/s 降到约 20/s）；第一轮日志里 `overlay-window-visibility visible=0 reason=idle` / `visible=1 reason=content` 成对出现，但第二轮 `windowHidden` 始终为 0——因为状态条（`StatusBarEnabled` 默认开，约 450×70 px）让窗口永远有内容，于是"空闲隐藏"实际用不上。

## 结论与后续选项

BitBlt 的同步全屏读取是"掉帧"的主因，改为 WGC 默认后大到 150 ms 的卡顿消失；剩下的是一个小而可测的合成/带宽代价（>20 ms 帧约多 1-3%）。要继续削，按性价比排序：

1. 让"无内容隐藏窗口"真正生效：状态条默认关闭，或状态条不显示时也允许隐藏——代价是少一个状态显示。
2. 缩小覆盖层窗口到实际绘制区域（现在是 450×70 的状态条也要合成整屏 2560×1440）；预计能降合成成本，但**未必**能恢复 Independent Flip（一个可见的分层窗口只要与游戏重叠就会挡住直出）。
3. 降低采集/发布频率（例如 20 fps），等比减少 WGC 的拷贝与回读流量。
4. 结构性改造：用 DirectComposition 取代 `WS_EX_LAYERED + LWA_COLORKEY`，这是唯一有机会让游戏恢复 Independent Flip 的路子，工作量也最大。
5. 直接接受：以本机实测，WGC 默认版的代价已远小于 BitBlt 版本（>100 ms 巨帧从 3 个降为 0）。

