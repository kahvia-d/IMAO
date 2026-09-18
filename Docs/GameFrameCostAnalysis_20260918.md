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
