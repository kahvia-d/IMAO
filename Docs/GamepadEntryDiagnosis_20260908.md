# LB 入口无响应排查（2026-09-08）

## 已定位根因与修复

用户使用诊断版复测后，18:25:33、18:25:35–36、18:25:40 三次都收到了 LB，并经过完整长按触发 `OpenAssistant`。原实现随后记录 `opened=True`，但实际前台仍是游戏 HWND 264752，下一次输入轮询就进入 `assistant-unfocused` 并关闭窗口。18:25:40 的详细顺序：

| 时间 | 事件 |
| --- | --- |
| 18:25:40.479 | 长按完成，分发 OpenAssistant |
| 18:25:40.513 | 创建窗口后立即声称 opened=True，前台仍为游戏 |
| 18:25:40.541 | 检测不到助手前台焦点，关闭刚创建的窗口 |

原先仅调用 `Window.Activate()`，并用会话布尔值代表打开成功，没有验证跨进程前台切换。既有完整服务测试的模拟游戏窗口与助手同属一个进程，因此没有覆盖这个差异。现场日志保留于 `out/gamepad-focus-20260908/user-gamepad-before.jsonl`。

修复使用独立 `GamepadWindowActivation`：验证目标归本进程及当前前台仍为游戏，普通激活后显式请求前台并等待确认；必要时先检测来源窗口响应，再进行一次临时输入队列附加与前台请求，立即解除。取消、过期、窗口改变或观察到第三方前台时停止。只有实际取得前台并完成两次确认，才公开手柄会话；失败明确记录原因，不再虚假报告 opened=True。

操作系统没有为前台切换提供原子“检查当前窗口再切换”或调用硬超时；临时输入队列附加也有阻塞风险，因此本实现不循环抢焦点、不在附加期间等待或执行 XAML 操作，并在 finally 解除。参考微软 [SetForegroundWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setforegroundwindow) 与 [输入队列附加的限制](https://devblogs.microsoft.com/oldnewthing/20080801-00/?p=21393)。

## 修复验证

- 旧版使用两个真实独立进程复现同样失败：游戏测试子进程 HWND 658604 保持前台，服务长按后声称 opened=True，下一轮因助手失焦关闭。首次日志的工具原文保留在 `out/gamepad-cross-process-baseline/baseline-first-run-tool-capture.txt`；文件说明了原日志后来被准备阶段失败重跑覆盖的来源情况。
- 修复版普通跨进程场景中，普通请求确实返回 False；一次临时输入队列附加后的请求成功，前台从游戏测试子进程变成助手窗口，解除结果 `DetachError=0`，最后才记录 opened=True。此测试直接覆盖现场遗漏的跨进程差异。
- 三项跨进程测试全部通过：正常交接；独立来源暂时无响应时 100 ms 探测失败、未附加输入队列且 opened=False；等待目标查询时切换到第三方窗口则取消打开。证据：`out/gamepad-cross-process-baseline/fixed-regression.log`。原有 25 组真实窗口测试也全数通过：`out/guide-window-runtime/guide-window-tests.log`。
- 新版标准构建、导航元数据与资源检查通过：`out/gamepad-focus-20260908/build-verified.log`。前两次构建仅因旧管理员进程占用相同内容的 apphost 启动器而复制失败；用户结束旧进程后标准流程完整通过，没有跳过构建检查。
- 修复后的真实游戏按键体验仍待用户使用新版验证；不把模拟按键的跨进程测试标为游戏内实测。

## 用户现场证据

- 用户反馈：点按、长按 LB 都没有反应。当前设计要求长按 600 ms，点按本身不打开助手。
- `LocalAppData/IMao-WinUI/Logs/events-20260908.jsonl` 的 18:08:14–18:11:45 会话中，手柄地图已识别为 `BigMap`；正常开图期间有 `focused=1`、`controlLayout=controller`、`controllerTriggerAnchors=2`、`controllerSlider=1`，并持续成功定位地图。
- 保存的配置为 `GamepadEnabled=true`、`GamepadControllerIndex=-1`、`GamepadEntryButton=256`（LB）。未修改用户配置。
- 单独进程的只读 XInput 检查中，只有槽位 0 连接，四轴在 -677 至 663 附近，明显小于当前 12000 死区；其余槽位均未连接。这排除了检查时的多设备选择错误及明显摇杆偏移，但不等于证明 WinUI 后台进程收到了用户的 LB。
- 旧版未将手柄读取与长按事件写入日志，不能从原生日志反推 LB 已收到或具体取消原因。

## 诊断改进

- 手柄状态区区分：核心未启动、设备断开、地图状态过期、画面不可确认、游戏失焦、等待松键、长按进度。
- 独立 `Logs/gamepad-YYYYMMDD.jsonl` 记录配置、设备和入口条件变化、肩键按下/释放、长按开始/结束、动作分发及窗口打开结果。稳定状态每 5 秒一条，不逐帧写入。
- 输入诊断包括当前槽位、按键和摇杆数值、采样间隔、地图上下文年龄、generation、地图可用性及前台窗口。
- 日志写入失败不会中断手柄输入；不与原生 events 文件竞争写入。

## 验证范围

- 真实 WinUI 进程中的读取实验：后台、前台、恢复后台三个阶段各 20 次，槽位 0 始终连接，四轴均保留 [-677, 663, 190, 385]，并非全零。原前台成功恢复。证据：`out/guide-window-runtime/gamepad-reader-probe.jsonl`。实验期间没有 LB 按下，因此这不能证明游戏内 LB 已被收到。
- 新增完整服务测试使用生产 `GamepadInputService` 的真实定时器和真实助手窗口，仅注入手柄样本、模拟核心响应和自有游戏窗口。约 31–32 ms 采样，满 600 ms 后依次完成目标查询、窗口注册、激活列表，并保持一次打开。证据：`out/guide-window-runtime/gamepad-service-tests.log`。
- ManagedRuntime 全套通过，包括既有 129 项手柄断言和新增 6 项日志验证。证据：`out/gamepad-entry-managed.log`。
- 完整真实窗口测试 25/25 组通过；包含新增的服务入口链路。证据：`out/guide-window-runtime/guide-window-tests.log`。
- 标准 Release 构建、6 个导航页面元数据、资源检查通过。证据：`out/gamepad-entry-build.log`。输出为 `x64/Release/IMao-WinUI.exe`，主 DLL 时间 18:23:40。

以上是定位根因之前的验证记录。当时缺少真实 LB 失败样本；后续用户复测已确认上述焦点交接根因。修复后的跨进程回归与真实游戏复测仍须分别记录，不能将模拟手柄样本等同于游戏内验证。
