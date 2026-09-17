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

## 待测

工具关 / 工具开（WGC）两段对照，用 PresentMon 取 `msBetweenPresents` 分布、丢帧数与 **`PresentMode`**（`Hardware: Independent Flip` vs `Composed`）。若工具开着时游戏从独立翻转变为合成，第 1 条机制即被证实，下一步是缩小覆盖层窗口（只覆盖实际绘制区域）或改用 DirectComposition，而不是继续微调 30 Hz 呈现。
