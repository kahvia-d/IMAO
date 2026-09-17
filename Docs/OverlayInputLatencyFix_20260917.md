# 2026-09-17 覆盖层输入延迟修复

## 现象与定位

玩家反馈在游戏中移动鼠标转视角"有延迟、时快时慢"。定位到覆盖层线程：全局低级鼠标/键盘钩子在 `imguiThread` 内安装（`IMao-Core/src/ImguiDraw/Items/DrawMarkerInteraction.cpp:888/890`，由 `IMao-Core/src/ImguiDraw/ImGuiOverWindows.cpp:353` 调用）。Windows 把低级钩子回调投递到安装线程的消息队列并等待该线程执行，而这条线程此前每帧只在帧首泵一次消息（`ImGuiOverWindows.cpp:370-376`），随后做图像跟踪、绘制与 `Present`，最后在 `FramePacer::WaitUntil` 里阻塞到下一个 16.667 ms 边界（常量 `kOverlayFramePeriod`，`ImGuiOverWindows.cpp:53`、`:542`）。结果是每个鼠标事件的实际延迟被量化到帧相位，并随该帧工作量抖动——与"时快时慢"的现象一致。

## 改动

- `IMao-Core/src/Runtime/FramePacer.h`：新增 `WaitUntil(deadline, pump)` 重载。它用高精度可等待定时器与 `MsgWaitForMultipleObjectsEx(..., QS_ALLINPUT, MWMO_INPUTAVAILABLE)` 一起等待，等待期间持续调用 `pump()` 泵消息，`pump()` 返回 false 时立即返回。原有的阻塞重载保留给不持有钩子的采集线程（`App.cpp:171/209`）。
- `IMao-Core/src/ImguiDraw/ImGuiOverWindows.cpp`：帧首、图像跟踪前、渲染前、帧尾等待统一走泵消息路径，帧尾节流不再把整帧时间交给阻塞等待；窗口位置校正失败与设备重建两条 `continue` 分支同样如此。
- 诊断：每 2 秒的 `overlay-motion` 记录新增 `inputGapMs=`，即该窗口内两次泵消息之间的最大间隔——也就是这条线程给玩家鼠标带来的最坏延迟，可直接从会话日志核对改动效果。

## 证据

- 新增 `IMao-Core/tests/FramePacerTests.h`（8 项检查，接入 `OptimizationTests.cpp`，`CMakeLists.txt` 为 `IMaoOptimizationTests` 补 `user32.lib`）：泵消息重载遵守截止时间、到期只泵一次、`pump()` 请求停止时立即返回，以及**投递到线程队列的消息会在截止时间之前很久就被处理**（`PostThreadMessage` + 记录每次泵的时间点验证，而不是用返回时间，因为等待仍会持续到截止时间）。
- `x64\Release\IMaoOptimizationTests.exe`：全部通过（含既有 900 余项检查）。
- 预期效果：`inputGapMs` 从改动前的一帧量级（≥16 ms，跟踪较慢时更高）降到个位数毫秒，剩余上界由单帧内最长工作段决定（图像跟踪实测约 1–2 ms/帧）。

## 边界与后续（需要实机 A/B）

- 本次只修输入服务路径，尚未实机确认手感。判断依据：会话日志里的 `inputGapMs` 与玩家主观延迟。
- 仍未处理、且都属于"改变游戏侧帧时间"的改动，需实机对比：
  1. 默认截图方式是 BitBlt，并以 60 Hz 调用 `PrintWindow` 抓游戏窗口（`App.cpp:168-210`、`WindowsCapture/BitBltCapture/BitBltCapture.cpp:92`），每次都会要求游戏窗口同步产出画面。可考虑仅在内置覆盖层需要画面时保持 60 Hz，其余时间降到识别周期（80 ms）。
  2. WGC 路径每帧新建 staging 纹理 + 阻塞式 `Map(..., D3D11_MAP_READ, ...)` + 帧回调内 `Present1(1, 0, ...)`（`WindowsCapture/WindowsGraphicsCapture/SimpleCapture.cpp:237-268`）。
  3. 覆盖层窗口以 `WS_EX_LAYERED + LWA_COLORKEY` 全屏 TOPMOST 60 Hz Present（`ImGuiOverWindows.cpp:274/349/532`）；另外设置里的"应用窗口兼容设置"写入的是**全局** `SwapEffectUpgradeEnable=0`（`IMao-WinUI/Helpers/BitBltRegistryHelper.cs:19-27`），目前没有恢复默认值的入口。
