# 工具对游戏帧率的影响：玩家现场现象与成因分析

## 1. 现象

玩家在**最新 release（2026.9.17.6）**上以正常玩法体验，同一场景前后两张截图：

| 截图 | 工具状态 | 游戏自报帧率 | 帧时间 |
| --- | --- | --- | --- |
| `player-tool-on.jpg` | 点击「开始探索」，工具运行中 | **99 fps** | 7.10 ms |
| `player-tool-off.jpg` | 点击「停止探索」，工具关闭 | **120 fps** | 8.19 ms |

- 采集分辨率 2560x1440，两次截图都是「小地图 / 草地 / 同一角色」的开放世界场景。
- 差距是 **120 → 99，约 17%**，即每帧多出约 1.8 ms。
- 注意 `99 fps × 7.10 ms ≈ 70%` 与 `120 fps × 8.19 ms ≈ 98%`：截图里那一刻的帧时间读数与
  自报 fps 并不自洽（游戏自己的平滑窗口 vs 瞬时值），**因此单张截图只能作为现象证据，
  不能作为量化结论**。真正的量化必须用 PresentMon 逐帧 `MsBetweenPresents` 在同一段
  游戏内容上做「关 / 开 / 关」三段对比，与 `Docs/GameFrameDropAnalysis_20260917.md`
  的方法一致。

## 2. 已有的历史结论（2026-09-17，同一台参考机）

`Docs/GameFrameDropAnalysis_20260917.md` 已经定位并修掉了一部分：

- **BitBlt（`PrintWindow`）同步整帧读取 31 ms 是主犯**：它直接打在游戏窗口的呈现路径上，
  造成 3 个 >100 ms 的游戏巨帧。已改为 WGC 默认，巨帧消失。
- 修改后**仍有残余**：>20 ms 的帧从约 1% 升到 3.7%；工具开着时游戏 100% 停留在
  `Composed: Flip`，工具关掉后约四成帧能走 `Hardware: Independent Flip`。
- 该文档第 71-77 行已经把后续选项按性价比列出来了，但**都还没做**。

本轮玩家报告的是 release 版本，也就是说：**BitBlt 那个主犯已经修掉了，玩家现在感觉到的
是那一份「残余」**。下面把它拆到代码级。

## 3. 玩家现场日志（本机 `%LOCALAPPDATA%\IMao-WinUI\Logs`）

不是估算，是玩家自己这次 session（`events-20260918.jsonl`，10:33-10:40）的记录：

```text
app-init            client=2560x1440
overlay-window-created  origin=0,0 size=2560x1440
capture-wgc-first-frame width=2560 height=1440
capture-wgc-frames  arrived=1735 published=1734 skipped=0
                    readbackAvgMs=21.38 readbackMaxMs=148.42
overlay-motion      renderFps=29.69 sourceFps=10.39 captureFps=10.39
                    presentSkipped=41 windowHidden=0
capture-cadence     captureAvgMs=3.56 captureMaxMs=5.11 capturePeriodMs=80
```

- 覆盖层窗口就是**整屏 2560x1440 的置顶分层窗口**。
- 覆盖层渲染循环**恒定 29.7 Hz**，与画面有没有变化、有没有标记无关。
- 状态条开启（`StatusBarEnabled=true`，玩家配置默认）时**永远有内容**，所以
  `windowHidden` 在开着工具、游戏在前台时始终为 0 → 这个置顶分层窗口**始终在场**。
- WGC 的**读回始终在跑**：`readbackAvgMs≈21-36 ms`，`readbackMaxMs` 高达 148-223 ms。

## 4. 成因（按置信度与影响排序）

### A. 整屏置顶分层窗口把游戏按在 DWM 合成路径上（机制确定，影响最大）

`ImGuiOverWindows.cpp:301` 创建的窗口是 `WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT`
的 **`WS_POPUP`、尺寸等于游戏客户区**（2560x1440）。只要它可见且与游戏重叠：

- 无边框全屏游戏本来可以走 `Hardware: Independent Flip`（MPO / 直出），不被 DWM 每帧重采样；
- 一个可见的分层窗口盖在上面，就把游戏压回 **`Composed: Flip`**：DWM 每个显示帧都要把
  游戏表面读出来、和自己的表面混合、再写回。这是**每显示帧**的固定成本，
  120 Hz 下就是每秒 120 次整屏混合。
- 玩家场景正好是 GPU 敏感的开放世界草地，游戏本来就贴着 120 fps 的预算（8.33 ms/帧）。
  每帧多 1-2 ms 的合成开销，恰好把 120 打成 99 - 这正是「差 17%」的量级。

**这是唯一能解释「帧率从 120 掉到 99」的机制**：这不是我们的 CPU 抢核，而是改变了游戏
自己的呈现路径。

### B. 覆盖层自己以 30 Hz 对整屏双缓冲交换链做 Clear + 全屏绘制 + Present

`ImGuiOverWindows.cpp:427-664` 的主循环：

- 即使内容没变，也照跑 `ImGui_ImplDX11_NewFrame` / `ImGui::NewFrame` / `ImGui::Render`；
- 真的 `Present` 时（内容变化或有状态条时几乎每帧）执行
  `ClearRenderTargetView` + `ImGui_ImplDX11_RenderDrawData` + `Present(0,0)`，
  目标是 **2560x1440 的 `DXGI_SWAP_EFFECT_DISCARD` 双缓冲**；
- 30 Hz × 14.7 MB ≈ **440 MB/s** 的额外 GPU 写入，加上第 4 节 A 的整屏混合。

`OverlayPacing::ShouldPresentFrame` 的内容哈希跳过确实生效了（`presentSkipped` 有值），
但只要状态条在，内容几乎每帧都变（帧时间毫秒数就在状态条里），**跳过实际很少命中**。

### C. WGC 的读回永不节流，帧池回调线程接近满载

`SimpleCapture.cpp:173-343` 的 `ProcessFrame` 每一帧都做：

1. `TryGetNextFrame()`；
2. `CopyResource` 到 staging 纹理（14.7 MB）；
3. `Flush()`；
4. **阻塞式 `Map(D3D11_MAP_READ, 0, ...)`** 等 GPU 拷完 → 实测 `readbackAvgMs≈21-36 ms`；
5. `mappedFrame.copyTo(ownedFrame)` → **第 4 份 14.7 MB 的 CPU 拷贝**；
6. `m_latestFrame = std::move(ownedFrame)`（这里是移动，不拷贝）。

`Direct3D11CaptureFramePool` 用 `CreateFreeThreaded(device, format, 2, size)`
（`SimpleCapture.cpp:49`），`FrameArrived` **按游戏呈现节奏逐帧投递，没有任何频率限制**。
我们的消费侧只需要 10-30 fps，却在为**每一次到达**付出一次整屏 GPU→CPU 读回。

而且 `FrameArrived` 只在上一帧被释放后才再次投递，所以这 21-36 ms 的读回**反过来把帧池
节流到约 30-45 fps**，游戏每秒 120 帧里有大量帧被丢弃-但丢弃不代表免费：拷贝与 Map 的
GPU 同步仍然发生。这条链路是持续的 PCIe / 内存带宽负载，且有 148 ms 的尖峰。

**SDK 里已经有正确的开关没有用**：`GraphicsCaptureSession.MinUpdateInterval`
（`IGraphicsCaptureSession5`，Win11 22H2+）可以直接告诉系统「我不要高于 N fps 的帧」，
从而在源头消掉绝大部分读回。

### D. 每帧重复的整屏拷贝

同一个捕获帧上存在多层全尺寸拷贝：

| 位置 | 内容 |
| --- | --- |
| `SimpleCapture.cpp:275` | `CopyResource` 到 staging（GPU） |
| `SimpleCapture.cpp:300` | `mappedFrame.copyTo(ownedFrame)`（CPU，14.7 MB） |
| `SimpleCapture.cpp:310` | 移动给 `m_latestFrame`（无拷贝） |
| `SimpleCapture.cpp:101` | `WaitForFirstFrame` 里 `m_latestFrame.copyTo(outputFrame)`（CPU，14.7 MB） |
| `App.cpp:443` | `result = temp(clientRoi)`（无拷贝，ROI 视图） |
| `App.cpp:1946` | `captured.image(region).clone()`（小地图/大地图区域） |

消费侧只要 10-30 fps，但**每一次读回都带一次 14.7 MB 的 `cv::Mat` 分配 + memcpy**。
在 30 fps 下是约 440 MB/s 的分配/拷贝抖动，正好压在游戏的 CPU 预算上。

### E. 后台线程的 CPU 争用（量级较小，但顺手指得出来）

- `Thread_DetectGameState` 固定 10 Hz 跑：`IsExistMinMap` 的任务图标 ROI
  `increaseImageResolution(..., 2.5)` 后 `SURF::create(80,6,4,true,true)` 提特征 + 暴力匹配；
  颜色罗盘探针；大地图控件布局探测；小地图 HUD 证据。
- `App::Start` 以 10-30 Hz 跑小地图裁剪 → SURF → 视觉定位（实测 `visual-local-tracking-time`
  0.7-1.4 ms，不贵），大地图打开时还有 `map-viewport` 的突发（历史上 ~100 ms）。
- 这些线程已经设为 `BELOW_NORMAL`（`ThreadPriority.h`，2026-09-17 那次改动的 C 项），
  所以它们只在游戏没吃饱的核上跑，**不是 17% 的主因**，但会放大第 4 节 A-E 的抖动。

### F. 被排除的怀疑

- **覆盖层是点击穿透的**（`WS_EX_TRANSPARENT`），不会吞输入；
- **覆盖层没有按帧 `SetWindowPos`**：`OverlayWindowBounds::Synchronize` 只在几何不一致时
  才调 `SetWindowPos`（`OverlayWindowBounds.h:13`），正常情况是纯 `GetWindowRect` 比较；
- **覆盖层交换链不是同一块 GPU 的额外设备**：它自己是 `D3D11CreateDeviceAndSwapChain`，
  与 WGC 的 capture 设备独立，没有互相阻塞；
- **鼠标钩子只在需要交互时安装**（`OverlayPacing::WantsMouseHook`），日志里
  `hooks=mouse=off keyboard=on` 证实了这一点。

## 5. 结论

玩家在 release 上感觉到的 120 → 99，**不是我们抢了 CPU，而是两件更结构性的事**：

1. **一个整屏大小的置顶分层窗口长期在场，把游戏从 Independent Flip 压回 DWM 合成**，
   让游戏每个显示帧都多付一次整屏合成（第 4 节 A / B）；
2. **WGC 的整屏 GPU→CPU 读回以游戏呈现节奏持续运行且不节流**，每秒数百 MB 的
   PCIe 与内存流量，加上每帧一次 `cv::Mat` 分配拷贝（第 4 节 C / D）。

历史文档里那句「剩下的不是我们的阻塞调用」只对**阻塞**成立；这两条是**带宽与呈现路径**
成本，不体现为某个 `xxxMs` 尖峰，所以上次没被认领，但它恰恰是每帧都付的钱。

## 6. 后续方向（按性价比）

1. **给 WGC 加 `MinUpdateInterval`**（只在我们需要的频率上要帧）。改动小、风险低、
   直接在源头砍掉第 4 节 C / D 的大部分流量。需要 `ApiInformation` 运行时探测，
   旧系统上回退到现在的行为。
2. **让覆盖层窗口只覆盖真正要绘制的区域**，而不是整屏。目标是减少整屏合成面积，
   并给「无内容时隐藏窗口」真正创造生效机会。注意：一个可见分层窗口只要与游戏重叠就会
   挡住直出，所以这一步**未必**能恢复 Independent Flip（历史文档已指出）。
3. **状态条与呈现频率解耦**：内容哈希跳过目前几乎不命中，因为帧时间文本每帧都在变；
   把「每秒都在变的文本」从哈希里排除，或让状态条按更低频率更新。
4. **合并拷贝链**：让消费侧直接读 staging 映射的视图（或复用缓冲），消掉每帧一次
   14.7 MB 的 `cv::Mat` 分配 + memcpy。
5. **结构性改造**：用 DirectComposition 取代 `WS_EX_LAYERED` + `LWA_COLORKEY`，
   这是唯一有机会让游戏恢复 Independent Flip 的路子，工作量最大。

## 7. 验收方式

必须用 PresentMon 在同一段游戏内容上做「关 / 开 / 关」三段对比（方法见
`Docs/GameFrameDropAnalysis_20260917.md` 第 40-42 行的工具与注意事项），
并同时记录工具侧 `capture-wgc-frames` 的 `readbackAvgMs` / `arrived` 与
`overlay-motion` 的 `renderFps` / `presentSkipped` / `windowHidden`，
这样每一条改动的效果都能单独归因，而不是只看游戏自报的 fps 数字。

### 7.1 用于归因的具体读数

| 改动 | 该看哪个读数 | 预期方向 |
| --- | --- | --- |
| 1 · 采集限流 | `capture-wgc-frames` 的 `arrived`、`readbackAvgMs` | 两者都下降；`applied=1 intervalMs=33.333` 出现在 `capture-wgc-rate-limit` |
| 3 · 状态条 | `overlay-motion` 的 `presentSkipped` | 在工具正常工作时明显上升（10:34 那次只有 41/2 秒） |
| 4 · 拷贝链 | `capture-cadence` 的 `captureAvgMs` 与整体 CPU 占用 | 小幅下降，且帧时间分布更紧 |

## 8. 实施状态（2026-09-18）

已在分支 `fix/overlay-game-frame-impact` 落地前三条里改动最小、风险最低的三项，
第 4 节 A / B（整屏置顶分层窗口与 30 Hz 整屏呈现）**尚未改动**：

- `7e965ac` 采集限流：`SimpleCapture` 在建立会话后设置
  `GraphicsCaptureSession.MinUpdateInterval = OverlayPacing::kCaptureMinUpdateInterval`（33.33 ms）。
  该属性是可选的（Windows 11 22H2+），运行时用 `ApiInformation::IsPropertyPresent` 探测，
  没有它就保持原来的不节流行为并记录 `applied=0`；有它则记录
  `capture-wgc-rate-limit applied=1 intervalMs=33.333`。新增两条策略断言保证这个间隔
  不会饿死已附着覆盖层的节奏，也不会是 0。
- `c4f7d9c` 状态条稳定：`App::Start` 改为按窗口累计帧时间、每秒最多发布一次均值，
  于是状态条文本在窗口之间保持稳定，`OverlayPacing::ShouldPresentFrame` 得以真正命中。
  发布的量仍是「每轮定位循环耗时」，语义与窗口大小不变。
- `119443f` 拷贝链：读回直接拷进保留下来的帧缓冲（尺寸不变时整个会话只分配一次），
  消费侧同样复用目标 Mat；因为缓冲是「覆盖」而不是「替换」，等待新帧改为按调用方
  已消费的序号判断，`App::Init` 读掉的启动帧由 `CaptureSequenceFilter` 精确跳过。
- `2dae30d` 把上面那条序号规则从 `App::Thread_Capture` 里提出来变成
  `OverlayPacing::CaptureSequenceFilter`，并补上基线、跳过与重复发布的用例。

以上四项都只经过构建与 `IMaoOptimizationTests`（全绿）验证，**没有做过实机帧率验证**。
按第 7 节的读数验收时，重点看 `readbackAvgMs` / `arrived` 是否随改动 1 下降、
`presentSkipped` 是否随改动 3 上升；如果改动 1 与 3 拿回的比例有限，就说明剩下的主要是
第 4 节 A（呈现路径）而不是带宽，需要进入方向 2 或方向 5。

## 9. 第一次实机验证结果（2026-09-18 11:28，玩家现场 53 秒）

玩家在同一台机器、`2560x1440`、同一个 open-world 场景上跑了一次完整 session，
结论是**帧率没有明显回升**。日志（`events-20260918.jsonl`，11:28:32 起）说明改动本身生效了：

| 指标 | 改动前（10:33 那次） | 改动后（11:28） |
| --- | --- | --- |
| WGC 到达频率 | 31.5 Hz（游戏每呈现一帧就来一帧） | **24-30 Hz（被限住）** |
| `readbackAvgMs` 起始 | 21-36 ms | **3.1-3.4 ms** |
| `readbackMaxMs` 尖峰 | **148 / 223 ms** | 85-88 ms |

- `capture-wgc-rate-limit applied=1 intervalMs=33.333000` 出现，限流确实生效，不是静默回退。
- 两个 148/223 ms 的大尖峰消失，这是这次改动实打实的收获。
- `presentSkipped` 没有明显变化。原因是呈现频率本来就被采集频率卡住（约 15 次/秒），
  所以「内容没变就不重画」本来就省不下多少——这一条在实机上收益有限。

### 9.1 本次改动引入并已修复的回归

`119443f` 把读回拷贝放进了 `m_frameMutex`。那个拷贝会等 GPU 几十毫秒，于是消费方拿锁也要等：

```text
captureAvgMs=13.61   captureMaxMs=71.70   slow=2
captureAgeMs=69 ~ 181 ms
```

`7102346` 已修复：拷贝改到只由帧池回调写的 scratch 缓冲，锁里只剩一次 `swap`。
下次会话应看到 `captureMaxMs` 回落到 10 左右。这一项属于**定位与叠加层延迟**，
不是帧率问题，但必须修。

### 9.2 由此得到的判断

采集路径比原来便宜了 3-7 倍、尖峰也消失了，**帧率却没有回来**。这排除了一件事：
掉帧不是「我们花了多少带宽」造成的。剩下的解释就是第 4 节 A——**一个可见的整屏置顶
分层窗口把游戏按在合成路径上**。但这一步还没有被独立验证过，所以先做第 10 节的对照实验，
再决定是否投入方向 2 / 方向 5 的改造。

## 10. 归因对照实验（诊断开关）

`0ae799c` 在「诊断」页加了一个开关：**隐藏叠加层窗口（仅采集）**。打开后采集、定位、
绘制与 present 全部照常运行，只有窗口本身不显示。于是可以做三组对照：

| 组 | 叠加层窗口 | 采集 | 用来回答 |
| --- | --- | --- | --- |
| 1 | 无（工具停止） | 无 | 游戏自身基准 |
| 2 | **隐藏**（开关打开） | 开 | 采集与定位本身的代价 |
| 3 | 显示（开关关闭） | 开 | 完整工具的代价 |

- 组 2 与组 3 的差值 = **窗口本身的代价**（即第 4 节 A）。
- 组 1 与组 2 的差值 = **采集与定位的代价**（即第 4 节 C / D / E）。

读数：`overlay-motion` 新增 `hiddenByDiagnostic=1`，用来区分「开关隐藏」与「空闲隐藏」。
`overlay-window-visibility` 会记录 `visible=0 reason=diagnostic-hidden`。

结论怎么用：

- 组 2≈组 1、组 3 明显更低 → 元凶是窗口，应当进入方向 2（缩小窗口）或方向 5（换贴法）。
- 组 2 明显低于组 1 → 采集链本身还有代价，回到第 4 节 C / D / E 继续削。
- 三组都低 → 我们此前的归因仍然不完整，需要重新取数。

## 11. 归因实验结果（2026-09-18 11:47-11:54）

玩家按第 10 节跑完了三组，体感与自报帧率一致：**1 比 2 比 3 丝滑，约 120 → 110 → 100 fps**。
日志确认开关与归因都成立：

```text
11:49:25  overlay-window-diagnostic   keepHidden=1
11:49:29  overlay-window-visibility   visible=0 reason=diagnostic-hidden
11:52:02  overlay-window-diagnostic   keepHidden=0
11:52:08  overlay-window-visibility   visible=1 reason=content
```

- 组 2 期间 `overlay-motion` 全程 `hiddenByDiagnostic=1 windowHidden=1`，
  但 `attachedFps≈29.76`：**采集、视觉定位、标记追踪确实都还在跑**，只是窗口不显示。
  这正是这次实验要保证的条件。
- 组 3 期间 `hiddenByDiagnostic=0 windowHidden=0 presentSkipped` 约 15-58，
  即恢复成「窗口在场 + 按内容变化呈现」。

### 11.1 代价被一分为二

| 组 | 窗口 | 采集与定位 | 实测帧率 |
| --- | --- | --- | --- |
| 1 | 无 | 无 | ~120 |
| 2 | 隐藏 | 开 | ~110 |
| 3 | 显示 | 开 | ~100 |

- **窗口本身 ≈ 10 fps**（组 2 → 3）：这是方向 2 / 方向 5 要处理的部分。
- **采集与定位 ≈ 10 fps**（组 1 → 2）：这是方向 1 / 2 / 4 要处理的部分。

两条几乎等量，所以「只做一条就够」是不成立的——这也解释了为什么第一批改动
（只削采集侧的带宽）拿不回帧率：它削的是其中一半里的一部分。

### 11.2 上一轮那个回归确实修好了

组 2 期间 `captureAvgMs` 稳定在 3.0-5.8、`captureMaxMs` 峰值约 20（个别 34），
不再是修复前的 `avg=13.61 / max=71.70`，`7102346` 的修复在实机生效。

### 11.3 还没回答的问题

现在的 10 fps 窗口代价有两个候选机制，实测还分不开：

1. **窗口在场**：一个可见的置顶分层窗口让 DWM 每显示帧都要合成整屏；
2. **每次呈现**：每次 `Present` 都让合成器把整屏重新混合一遍（组 3 约 12-15 次/秒）。

`0f98844` 加了第二个开关（**暂停叠加层画面更新**）：窗口继续显示，但只有状态条时
约每 3 秒才 present 一次，其余照常运行。于是：

| 组 | 窗口 | 呈现 | 回答 |
| --- | --- | --- | --- |
| 4 | 显示 | 每 3 秒约 1 次 | 若组 4≈组 2 → 代价来自**呈现次数** |
| 3 | 显示 | 12-15 次/秒 | 若组 4≈组 3 → 代价来自**窗口在场** |

- 组 4≈组 2 → 只要降低呈现频率就能拿回大部分，改动小（方向 3 的加强版）。
- 组 4≈组 3 → 降频无用，必须缩小窗口（方向 2）或换贴法（方向 5）。

读数：`overlay-motion` 新增 `presentHeld=` 与 `holdDiagnostic=1`，
`overlay-window-visibility` 不再出现 `reason=idle`（暂停期间窗口始终保持显示）。

## 12. 归因实验结论（2026-09-18 12:07-12:08）

第四组实测**约 100 fps**，与组 3 相同。日志确认开关生效且测量条件成立：

```text
12:07:21  overlay-present-diagnostic  holdPresent=1
12:07:27  overlay-motion  holdDiagnostic=1 presentHeld=54  windowHidden=0 hiddenByDiagnostic=0
12:08:25  overlay-motion  holdDiagnostic=1 presentHeld=0   windowHidden=0 hiddenByDiagnostic=0
```

`presentHeld` 每三秒积累约 60 帧，也就是 `Present` 从约 12-15 次/秒降到约 0.3 次/秒，
而**窗口全程 `windowHidden=0`**。帧率没有任何回升。

### 12.1 最终归因

| 组 | 窗口 | 呈现 | 采集+定位 | 帧率 |
| --- | --- | --- | --- | --- |
| 1 | 无 | — | 无 | ~120 |
| 2 | 隐藏 | — | 开 | ~110 |
| 3 | 显示 | 12-15 次/秒 | 开 | ~100 |
| 4 | 显示 | **约 0.3 次/秒** | 开 | **~100** |

**结论：这 10 fps 与刷新次数无关，完全来自「一个可见的置顶分层窗口在场」。**
只要它显示着（哪怕几乎不刷新），DWM 就必须把游戏留在合成路径上。

这也**同时否掉了方向 2**：缩小窗口减少的是合成面积，但「必须合成」这件事不变，
按组 4 的结果，面积不是这里的成本项。

### 12.2 因此只剩两条路

1. **让窗口在不需要时真正消失**。现有代码已经有这个机制（`OverlayPacing::ShouldHideIdleOverlay`
   会在无内容 1 秒后 `SW_HIDE`），但状态条让它永远有内容，永远不触发。
   本文档第 4 节 B 记录的现象就是这个。所以这条路的实质是**取舍**：
   不显示状态条 → 窗口能真正隐藏 → 拿回约 10 fps。
   组 2 已经证明「窗口隐藏时」帧率确实是 110。
2. **换掉贴法**（DirectComposition 取代 `WS_EX_LAYERED` + `LWA_COLORKEY`，方向 5）。
   这是唯一有机会**既保留常显 UI 又恢复 Independent Flip** 的路子，工作量与风险最大。

采集侧的另外 10 fps（组 1 → 2）仍然独立存在，无论选哪条路都还要另外处理。

### 12.3 路 B 的前提必须先验证（`3216319`）

方向 5（DirectComposition）在本文档第 6 节沿用了历史文档的说法：「唯一有机会恢复
Independent Flip 的路」。**这个说法从未被验证过**，而组 4 的结果反而对它不利——
DirectComposition 的表面**同样在合成树里**，换贴法并没有让窗口「不在场」。

所以先不投入完整改造，而是加了最小探针 `IMaoOverlayPresentProbe`
（`IMao-Core/tools/OverlayPresentProbe/`，含 README）：

- 三种模式：`none`（无窗口基准）、`dcomp`（`WS_EX_NOREDIRECTIONBITMAP` + DirectComposition
  visual）、`layered`（今天用的 `WS_EX_LAYERED` + `LWA_COLORKEY`）；
- 只有一块与状态条同尺寸同位置的可视色块，**不含采集、定位、ImGui**——
  那些已经单独测出约 10 fps，放进来会把要测的效应盖掉；
- `scripts/Test-OverlayPresentPath.ps1` 一次录完三段并打印 `PresentMode` 分布
  （需要管理员权限，PresentMon 要建 ETW 会话）。

判读：

| 观察 | 结论 |
| --- | --- |
| `dcomp` 段出现 `Hardware: Independent Flip`，`layered` 段为 `Composed: Flip` | 前提成立，值得做完整改造 |
| 两段都是 `Composed: Flip` | 换贴法不能恢复独立翻转，方向 5 被探针否决 |

这一步的价值在于：**用几小时的验证代替几天的改造**，且无论结果如何都不浪费。

## 13. 探针第一次运行结果（2026-09-18 12:23）——两个假设被推翻

三段数据（按 `phases.txt` 时间戳切分 `overlay-present-path.csv`）：

| 段 | 帧数 | fps | p50 帧时间 | p99 | >20 ms | PresentMode |
| --- | --- | --- | --- | --- | --- | --- |
| `none` | 5484 | **121.9** | 4.86 ms | 19.2 | 0.57% | Hardware: Independent Flip **100%** |
| `dcomp` | 5054 | **112.3** | 8.33 ms | 20.17 | 1.25% | Hardware: Independent Flip **100%** |
| `layered` | 5334 | 118.5 | 8.12 ms | 19.25 | 0.26% | Hardware: Independent Flip **100%** |

第三段无效（见 13.1），但 `none` 与 `dcomp` 两段有效，而它们推翻了两件事。

### 13.1 `layered` 段是无效的（探针的 bug）

玩家注意到**第三段看不到标记色块**——这是对的。`layered` 模式**什么也没画**，而
`SetLayeredWindowAttributes(..., LWA_COLORKEY)` 把纯黑当透明色，于是那个窗口完全透明，
**等于不在合成里**，与 `mode=none` 是同一件事。所以 118.5 fps 这个数不能用来代表
「可见分层窗口」，该段必须重测。

已修（`3562c83`）：`layered` 模式现在用 GDI 画出与 `dcomp` 相同的可见色块
（GDI 画的正是 colorkey 窗口真正拥有的重定向表面）。

### 13.2 假设一被推翻：DirectComposition 并没有恢复独立翻转

`dcomp` 段**全程就是 `Hardware: Independent Flip`**（100%，没有一帧是 Composed）。
也就是说：**一个可见的 DirectComposition 叠加层并不会把游戏踢出独立翻转**——
它本来就在独立翻转上。

这直接否掉了「换贴法能恢复独立翻转」的整个前提：**没有东西需要恢复**。
同时它也说明，方向 5 能带来的收益不是"恢复翻转"，而只是"合成更便宜"这一个小得多的东西。

### 13.3 假设二被推翻：可见覆盖层的代价不来自失去独立翻转

`dcomp` 段仍然掉了 **9.6 fps**（121.9 → 112.3），p50 帧时间 4.86 → 8.33 ms，
**但游戏全程都在独立翻转上**。所以这 9.6 fps 与 `PresentMode` **无关**：
它是一个可见的全屏合成表面本身占用的 GPU，而不是"被踢出快路"的惩罚。

这条同时说明：`Docs/GameFrameDropAnalysis_20260917.md` 里「工具开着时游戏 100% 停留在
`Composed: Flip`」这个结论**与今天的实测不一致**。那批数据来自 2026-09-17 的旧构建，
且当时游戏的呈现模式本身就在中途变化（该文档第 53 行自己记录了这点）。**在处理任何呈现路径
改造之前，必须先用 PresentMon 重新测一次真实覆盖层的 PresentMode。**

### 13.4 还有一个测量差异必须消除

探针第一轮默认用 `DwmFlush()` **按显示刷新率**刷新（`--hz=0`），而真实覆盖层是约 30 Hz。
在 165 Hz 上每显示帧都更新一个全屏合成表面，是比真实情况严苛得多的实验。
`3562c83` 已把默认改成 30 Hz（与真实覆盖层一致），`--hz=0` 保留用来观察最大值。

### 13.5 下一轮要测什么

用修好的探针重跑一次完整三段（默认 30 Hz）：

1. `layered` 现在可见了——**这次 `layered` 对 `dcomp` 才是真正的"两种贴法"对比**；
2. 如果两者 PresentMode 都是 Independent Flip 且帧率相近，那么**两种贴法没有实质差别**，
   方向 5 应当被放弃，转而调查真实覆盖层与探针的差异（采集、定位、呈现频率）；
3. 无论结果如何，都还要补一次**真实覆盖层（组 3 配置）的 PresentMon 测量**，
   才能知道今天到底该不该动呈现路径。

## 14. 探针第二次运行结果（2026-09-18 12:45）——结论反转

修好两个 bug 后重跑，三段几何完全一致（`block=999,22 562x87`，两段各 1350 次 present /
45 秒 = 30 Hz），数据可用：

| 段 | 帧数 | fps | p50 | p95 | p99 | >20 ms | PresentMode |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `none` | 5286 | **117.5** | 7.17 ms | 18.03 | 19.1 | 0.34% | Independent Flip **100%** |
| `dcomp` | 5206 | **115.7** | 8.17 ms | 18.51 | 19.57 | 0.61% | Independent Flip **100%** |
| `layered` | 4982 | **110.7** | 8.39 ms | 20.07 | 21.31 | **5.4%** | Independent Flip **100%** |

### 14.1 被证实：没有"独立翻转"需要恢复

三段**全部是 `Hardware: Independent Flip` 100%**。所以「覆盖层把游戏压回合成」这个前提
**不成立**——今天这个游戏 + 这个覆盖层形态下，游戏一直在独立翻转上。
方向 5 里"恢复快路"这一半的收益是 **0**，因为从来没有丢过。

### 14.2 被推翻（朝有利方向）：换贴法确实有可测收益，但只有一半

`layered` → `dcomp`：

- fps **110.7 → 115.7**（+5.0 fps，+4.5%）
- `>20 ms` 的帧 **5.4% → 0.61%**（少约 9 倍）

`>20 ms` 这一列才是玩家能感觉到的"卡顿"。5.4% 意味着**每秒有 6 帧超过 20 ms**，
而 `dcomp` 只有每秒约 0.7 帧。所以换贴法的收益**主要不是平均帧率，而是帧时间的平稳度**。

### 14.3 尚未解释：真实覆盖层的窗口比探针贵得多

把这次数据和第 11 节的四组实验对齐：

| 对照 | 代价 |
| --- | --- |
| 探针的 `dcomp` 叠加层（同等尺寸、同 30 Hz） | **1.8 fps** |
| 探针的 `layered` 叠加层（同等尺寸、同 30 Hz） | **6.8 fps** |
| **真实覆盖层窗口**（组 2 → 组 3 差值） | **约 10 fps** |

也就是说：**真实覆盖层的窗口比探针里"等价"的窗口多花约 3-8 fps**，
而且真实覆盖层到底是哪种 PresentMode **从未用 PresentMon 测过**。
探针没有复现真实覆盖层的某些东西——候选差异是
`DXGI_SWAP_EFFECT_DISCARD`（探针用 `FLIP_SEQUENTIAL`）、每帧整屏 Clear + ImGui 全屏绘制、
以及 `RefreshRate` 被写死为 60 等。

### 14.4 下一步

1. **用 PresentMon 测一次真实覆盖层**（组 3 配置）。这是唯一还没做过的关键测量，
   也是判断"该不该动呈现路径"的前提。
2. 如果真实覆盖层确实在 `Composed`，说明探针漏掉了它的某个特征，先把那个特征找出来再谈改造。
3. 如果真实覆盖层也在 `Independent Flip`，那么方向 5 的天花板就是**约 5 fps + 消除 5% 的
   20 ms 尖峰**；这时更值得先做的是**采集与定位那 10 fps**（组 1 → 组 2），
   它的收益上限更大，而且与呈现路径完全独立。
