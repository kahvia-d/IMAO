# 2026-09-17 覆盖层输入延迟修复

## 现象与定位

玩家反馈在游戏中移动鼠标转视角"有延迟、时快时慢"。定位到覆盖层线程：全局低级鼠标/键盘钩子在 `imguiThread` 内安装（`IMao-Core/src/ImguiDraw/Items/DrawMarkerInteraction.cpp`，由 `IMao-Core/src/ImguiDraw/ImGuiOverWindows.cpp` 的 `start()` 调用）。Windows 把低级钩子回调投递到安装线程的消息队列并等待该线程执行，而这条线程每帧还要做图像跟踪、绘制与 `Present`，最后在 `FramePacer::WaitUntil` 里等待到下一个帧边界。结果是每个鼠标事件的实际延迟被量化到帧相位，并随该帧工作量抖动。

实机日志（169 个 2 秒窗口）把责任段定死了：

| 段 | 中位 | 最大 | 说明 |
|---|---|---|---|
| `presentMs` | 24 ms | 32 ms | `ImGui::Render` + `Present(0,0)`：分层窗口 + `DXGI_SWAP_EFFECT_DISCARD`，每次呈现都要等合成器取帧 |
| `trackMs` | 3 ms | 42 ms | 跟踪 + 绘制（其中 `motionMs` 仅 2–3 ms，跟踪器本身不慢）|
| `boundsMs` | 0 | 232 ms | 窗口位置同步的偶发停顿 |
| `waitMs` | 17 ms | 19 ms | 节流睡眠（输入到达会唤醒，不产生延迟）|
| `inputGapMs` | 27 ms | 304 ms | 真正会憋住钩子的量，几乎全由 `presentMs` 决定 |
| `captureAvgMs` | 34.6 ms | 36.5 | 每次 `PrintWindow` 抓游戏窗口的成本，实测约 28 次/秒 |

## 改动

1. **输入服务不再被帧工作阻断**（`Runtime/FramePacer.h`、`ImguiDraw/ImGuiOverWindows.cpp`）：新增 `WaitUntil(deadline, pump)` 重载，用高精度定时器与 `MsgWaitForMultipleObjectsEx(..., QS_ALLINPUT, MWMO_INPUTAVAILABLE)` 一起等待并在等待期间持续泵消息；帧首、图像跟踪前、渲染前、帧尾统一走该路径，两条 recovery 分支同样如此。
2. **鼠标钩子按需安装**（`Runtime/OverlayPacing.h`、`Items/DrawMarkerInteraction.cpp`）：只有大地图发布了可点区域（`mapInteractive` 且 `regionsAt` 在 500 ms 内）时才装 `WH_MOUSE_LL`，其余时间完全卸载；已经由钩子接管的拖拽会保留钩子直到抬起，游戏不会收到"没有按下过的抬起"。未开大地图时（也就是玩家转视角的时候）本工具**一个鼠标钩子都没有**，额外延迟为零。键盘钩子仍需常驻（配置的指引/完成键要在游戏之前吃掉），但它的延迟现在只由 `trackMs`（约 3 ms）决定。
3. **覆盖层呈现改为 30 Hz**（`OverlayPacing::kFramePeriod`，原来 16.667 ms）：标记位置本身只按采集速率（约 30 Hz）变化，60 Hz 呈现只是反复向合成器排队并阻塞本线程；降到 30 Hz 后 `presentMs` 预期从 24 ms 降到接近 0。
4. **采集按需降频**（`OverlayPacing::CapturePeriod`、`App.cpp` 的 `Thread_Capture`）：内置覆盖层正在绘制标记（或大地图/小地图可见）时保持 33.3 ms，否则回到识别周期 80 ms —— 后者即上游项目的做法，可把"要求游戏额外同步产出画面"的次数减少约 2/3。
5. **WGC 路径修掉自身缺陷**（`WindowsCapture/WindowsGraphicsCapture/SimpleCapture.cpp`）：去掉回调内 `Present1(1, 0, ...)`（那个交换链表面没有任何消费者，却让回调每次阻塞到 vblank）；回读不再经由交换链后缓冲、不再每帧新建 staging 纹理；`Map` 改用 `D3D11_MAP_FLAG_DO_NOT_WAIT`，GPU 未完成时跳过该帧而不是等它。
6. **诊断**：`overlay-motion` 增加 `boundsMs/trackMs/motionMs/presentMs/waitMs` 与 `hooks=mouse=… keyboard=…`；`capture-cadence` 增加 `captureAvgMs/captureMaxMs/capturePeriodMs`。

## 证据

- `IMao-Core/tests/FramePacerTests.h`（9 项）：泵消息重载遵守截止时间、到期只泵一次、`pump()` 请求停止时立即返回、投递到队列的消息在截止时间之前很久就被处理。
- `IMao-Core/tests/OverlayPacingTests.h`（9 项）：没有地图交互时不装鼠标钩子、发布过的区域在宽限期内保留钩子、宽限期后移除、拖拽中即使地图关闭也保留、采集周期在有/无覆盖层时分别为 33.3/80 ms。
- `x64\Release\IMaoOptimizationTests.exe`、`IMaoMarkerTests.exe`、`IMaoRoutePlanningTests.exe`、`IMaoResourceSnapshotTests.exe`（58 项）全部通过。
- 实机验收：`overlay-motion` 里游戏内应显示 `hooks=mouse=off keyboard=on`；开大地图时变为 `mouse=on`；`presentMs` 应降到个位数、`inputGapMs` 随之降到几毫秒；`capture-cadence` 的 `capturePeriodMs` 在无覆盖层时为 80、有覆盖层时为 33.3。

## 边界与后续

- 仍需实机确认手感与交互（点击点位、框选、Esc 接管、快捷键、指引窗口）。
- 仍未处理、可继续优化的项：
  1. `PrintWindow` 单次约 35 ms 的**绝对成本**（`WindowsCapture/BitBltCapture/BitBltCapture.cpp:92`）没有变化，本次只降低了调用频率。设置里已有"Windows Graphics Capture"选项，其实现本次也一并修好，建议实机 A/B 两种截图方式的帧时间与手感；若 WGC 明显更好，可考虑改默认值。
  2. 覆盖层窗口仍是 `WS_EX_LAYERED + LWA_COLORKEY + DXGI_SWAP_EFFECT_DISCARD`。若 30 Hz 之后 `presentMs` 仍偏高，下一步是换成 DirectComposition + flip model（每帧全屏 blt 变为翻转），这属于渲染管线改造，需实机看到画面才算验证。
  3. 偶发 `boundsMs=232 ms`（`Runtime/OverlayWindowBounds.h` 的 `SetWindowPos` 同步）尚未处理。
- 同一个问题上已经处理的相关项：设置里的"应用窗口兼容设置"写入的是**全局** `SwapEffectUpgradeEnable=0`（`IMao-WinUI/Helpers/BitBltRegistryHelper.cs`），此前没有恢复入口，会让所有 Direct3D 程序停留在较旧的合成路径。现在设置里提供"恢复图形默认设置"，只删除这一个值、保留其他 Windows 图形偏好，清空后删除该值（`ManagedRuntime` 测试覆盖两种转换）。
