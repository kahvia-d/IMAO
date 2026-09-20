# 2026-09-18 覆盖层一直停在「等待定位」：BitBlt 回退一帧都发布不出去

## 现象

玩家「绿风」在 2026.9.18.1 上点「开始探索」后，游戏内状态条一直显示 `IMao · 等待定位` / `等待游戏界面`，直到退出都没有出现任何标记。开发机在截图方式选 BitBlt（或 Windows Graphics Capture 启动失败自动回退 BitBlt）时同样如此。

两份日志（同一份 2026.9.18.1 安装，玩家 4K 全屏 3840x2160）：

| 检查项 | 玩家 09-17（旧版） | 玩家 09-18（2026.9.18.1） |
| --- | --- | --- |
| 实际截图方式 | `first-frame-wait capture=bitblt ready=1` | `first-frame-wait capture=bitblt ready=1` |
| `capture-cadence` 的 `fps` | 2292 条**全部 > 0**（约 8–10） | 352 条**全部 = 0** |
| `overlay-motion` 的 `captureFps` / `captureAgeMs` | 有值 | `0` / `-1`（从未收到过帧） |
| `visual-localization-submit` | 2466 条 | **0 条** |
| `game-state` / `overlay-visibility` | 2710 / 934 条 | **0 条** |
| `app-ready` | `snapshot=available` | `snapshot=available`（启动首帧是好的） |

关键否定证据：两份日志里都**没有** `capture-frame-error` 或 `capture-frame-rejected`，`capture-cadence` 的 `captureAvgMs` 在玩家机器上是 45 ms 左右（BitBlt 正常在跑），`first-frame-wait ... ready=1`。所以**采集本身是成功的，被丢掉的是「发布」这一步**。同一个玩家、同一个 BitBlt 回退路径在旧版上是好的 —— 这是 2026.9.18.1 引入的回归。

## 原因

`v2026.9.17.6..v2026.9.18.1` 之间，`App::Thread_Capture` 的发布判据从

```cpp
if (sequence == 0) sequence = lastSequence + 1; // PrintWindow has no source sequence.
if (sequence != lastSequence) { lastSequence = sequence; publish(...); }
```

换成了 `OverlayPacing::CaptureSequenceFilter`：

```cpp
if (sequence == 0) sequence = lastSequence + 1;
if (sequenceFilter.Accept(sequence)) { lastSequence = sequence; publish(...); }
```

`Accept` 的规则是「第一帧只当基线、不发布」（为了跳过 `App::Init` 已经消费过的那一帧），这对**会自己给帧编号**的 Windows Graphics Capture 是正确的。但这个循环对**不编号**的 PrintWindow 是自己造号的，而造号用的 `lastSequence` 只在「接受」分支里更新：

| 迭代 | `GetMatSnapshot` 回传 | 循环合成出的 `sequence` | `Accept` | 结果 |
| --- | --- | --- | --- | --- |
| 1 | 0 | `0 + 1 = 1` | 记基线 `1`，`1 == baseline` | 拒绝，`lastSequence` 仍是 0 |
| 2 | 0 | `0 + 1 = 1` | `1 == baseline` | 拒绝 |
| 3… | 0 | `1` | `1 == baseline` | 拒绝（永远） |

**整个会话一帧都不会发布。** 消费者要求「新的 frameId」才会做事：检测线程的 `liveCapture` 永远为假，于是每轮都 `mapUiState.Reset()`、`isOpenMap=false`、`RuntimeStatus::SetGameState("unknown")`，而定位状态停在 `waiting` —— 状态条上就是「等待游戏界面」+「等待定位」。这正是玩家看到的现象。

`OverlayPacingTests` 漏掉这条路径的原因：原先的用例直接调 `filter.Accept(7)` / `filter.Accept(8)`，只验证了过滤器自身，没有模拟调用点「用上一帧编号 + 1 造号」的做法，而第一个被拒绝的帧恰好让造号永远停在同一个值。

## 改动

1. **`IMao-Core/src/Runtime/OverlayPacing.h`**：新增 `CaptureFrameSource`，把「编号后端 / 不编号后端」两种帧号来源放在一处决定。编号后端走 `CaptureSequenceFilter`（照旧跳过启动帧）；不编号后端由它自己单调编号，**永不拒绝**，且每次给出的 id 都是新的。
2. **`IMao-Core/src/App/App.cpp`**：`Thread_Capture` 改用 `CaptureFrameSource`，删掉循环里的 `lastSequence`。
3. **诊断**：`capture-cadence` 增加 `captureEmpty=`（采到的像素不可用：空图或几何不匹配）与 `captureUnpublished=`（帧号判据拒绝）。这两种原因从外部看完全一样（都是覆盖层永远等不到游戏画面），上次定位就卡在区分它们上。
4. **诊断**：`CaptureSnapshot::StartCaptureFromItem` 逐步记录 `capture-step-error step=create-session|create-surface|cursor|border|start`。玩家日志里 Windows Graphics Capture 的失败只有 `capture-item-error hr=0x80004002`（`E_NOINTERFACE`），而这一步之间有好几个调用，无法判断是哪一个。
5. **测试**：`IMao-Core/tests/OverlayPacingTests.h` 的两个用例都改为走 `CaptureFrameSource`，其中新增的用例断言「不编号后端连续三帧都能发布，且 id 互不相同」。

## 证据

- `IMaoOptimizationTests`（Release）全绿：`All optimization tests passed.`
- `IMao-CoreHost`（Release，VS 2022 配置 `out/build/windows-x64-release-vs144`）构建通过。
- 玩家日志逐项核对见上表：09-17 同一路径正常、09-18 全程 `fps=0`，且无采集错误 —— 与代码推断一致。

## 边界与后续

- **回退到 BitBlt 是设计行为**，不是本次的问题。玩家机器上 Windows Graphics Capture 从未启动过：`capture-wgc-rate-limit applied=0 reason=unsupported`（该系统没有 `MinUpdateInterval`，属于较旧的 Windows 版本）+ `capture-item-error hr=0x80004002`。这个回退在 09-17 也一样发生，只是那条路径当时还能工作。本次修复让这类机器恢复可用，但它们付的是每次 `PrintWindow` ≈ 45 ms（4K 全屏）的成本。
- **下一步要看 `capture-step-error` 报的是哪一步**。若报的是 `border`（`IsBorderRequired` 属于 `IGraphicsCaptureSession2`，旧系统上不存在），那么「旧系统上修复 WGC」并不是免费的：没有这个开关时系统会在被采集的窗口上画黄色采集边框。要不要为了 WGC 接受那个边框，是一个独立的产品决定，本次不动，只把事实记下来。
- 未做：Windows Graphics Capture 启动失败时玩家只看到一条系统通知（`Windows Graphics Capture 启动失败，已自动改用 BitBlt 捕获画面`），没有别的可见提示。
