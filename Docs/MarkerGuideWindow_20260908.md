# 攻略浮窗开关与完成交互

后续用户截图确认仍有顶部白色细条：本页早先的 `HasTitleBar`/样式验证不足以排除可见非客户区。最新复现、9 像素到 0 像素的修复、左侧定位及翻页快捷键验证见 [攻略白边、默认位置与图片翻页](MarkerGuideLayout_20260908.md)。

## 已实现

- 移除系统标题栏和边框，在点位标题旁保留关闭按钮；可拖动点位标题区域移动窗口。
- 攻略快捷键（默认 F8）再次按下关闭浮窗，在游戏和浮窗获得焦点时均可响应。注册或攻略加载尚未完成时也可取消打开。
- F8 从关闭状态打开时，使用关联请求 `markerGetRouteGuide` 在核心路线锁内读取当前档案的第一个未完成、未跳过目标，不使用可能滞后的 WinUI 路线快照或按键事件中的旧目标。无剩余目标时保持关闭。查询过程中收到完成事件则重新查询，第二次 F8 或显式点选可取消尚未完成的打开操作。
- 点击「标记完成」，或在攻略浮窗获得焦点时按完成键（默认 Z），保存成功后关闭浮窗。保存失败保留窗口并提示；完成键固定设为已完成，不会撤销已有完成记录。
- 游戏获得焦点时沿用附近点位完成规则；其成功完成事件仅关闭身份相同的攻略浮窗。其他点位、档案、场景的完成不会影响当前浮窗。
- 浮窗接管的按压通过原子状态共享给附近完成轮询。窗口隐藏、长按重复和按键释放都不会让同一次按压再次完成附近点，下一次全新未接管的按下才解除该键的拦截。
- 显式撤销完成只更新状态。攻略、图片、注册和完成请求绑定窗口会话代次；旧请求不会打开已关闭窗口或关闭重新打开的同一点位。
- 原生注册携带窗口句柄、档案、场景、点位 ID 和会话代次。切换点位时先隐藏旧内容，再注册新身份，避免显示与完成操作不一致。
- 核心断开或重启时，本地关闭浮窗并取消排队请求；不会为了注销旧窗口自动重启核心。
- 使用指南同步展示当前自定义快捷键与上述焦点规则。

## 首次离线验证（独立预览目录）

2026-09-08 本机 Release 验证通过：

- 常规 `Build-IMao.ps1` 完成本次 C++ 对象编译，链接阶段被正在运行的旧版 `x64/Release/IMao-CoreHost.exe` 占用，报 LNK1104。
- 保留当前运行的工具，以该次 CMake 生成的完整响应文件和链接参数输出独立核心；WinUI 使用独立 BaseOutputPath 构建。新版输出位于 `out/marker-guide-preview/x64/Release/`，WinUI 为 0 错误，保留既有警告。
- 6 个 WinUI 页面导航元数据检查通过；资源探针 `resourcesReady`、`viewportReady`、`visualReady` 均为 true。
- 原生优化、标记、路线与攻略协议回归通过；托管与真实 CoreHost 通信测试共 331 项 PASS。
- 回归覆盖快速开关、注册/本地/在线加载中关闭、失败保留、成功关闭、撤销不关闭、同一点位重新打开后的旧代次隔离；原生及 IPC 覆盖完整/部分注册身份、非法句柄、精确整数代次、完成身份回传，以及非法关联字段不改变内存快照、修订号或磁盘进度。

复现脚本：`out/Validate-MarkerGuideWindow.ps1`；测试阶段使用 `-TestsOnly`，仅更新原生时使用 `-NativeOnly`。增量原生构建以 CMake 生成的对象目标更新依赖，再使用同一响应文件重新链接独立核心。脚本使用独立应用数据目录，未更改用户点位进度或当前运行的窗口。

证据：`out/marker-guide-window-build.log`（常规输出被占用）、`out/marker-guide-window-isolated-build.log`、`out/marker-guide-window-input-build.log`，以及 `out/marker-guide-window-runtime/` 下的 `native-objects-build.log`、`native-link.log`、`winui-build.log`、`winui-navigation.log`、`resource-check.log`、`native-tests.log`、`marker-tests.log`、`route-planning-tests.log`、`managed-tests.log`。

## 本次问题复查与常用目录更新

用户报告再次 F8 像是重新打开、完成后又打开同一攻略。本次检查发现两个需要分别处理的问题：

- 上次常规输出被运行实例占用，修复只交付到了独立预览目录。本次构建前常用目录的 WinUI DLL 仍为 14:45:26、核心为 14:49:35；不能仅凭窗口症状判断用户运行了哪一份。
- 原打开逻辑用按键事件的目标与 WinUI 路线快照比较。完成事件比合并后的路线状态优先发送，两者都可能仍指向刚完成的点。已改为核心权威查询，并测试完成事件插入查询期间的重查与取消。

2026-09-08 本次 `Build-IMao.ps1 -Parallel 2` 成功更新常用 `x64/Release/`：最后一次 WinUI DLL 为 15:50:29，核心为 15:44:32。构建 0 错误；既有可空性警告保留。6 个页面导航检查及三个资源就绪标志通过。

`Test-Runtime.ps1 -OutputDirectory out/marker-guide-followup-runtime -Parallel 2` 通过全部原生回归和 341 项托管/真实 CoreHost 通信检查。新增真实 IPC 验证完成 A 后即使携带旧目标和旧路线 ID 也返回 B、全部完成和退出后返回空、跳过与撤销跳过、档案不符拒绝，以及查询不发出打开事件、不改变进度文件或活动路线文件。证据位于 `out/marker-guide-followup-build.log`、`out/marker-guide-followup-tests.log` 和 `out/marker-guide-followup-runtime/`。

## 真实 WinUI 窗口验证

新增 `Tests/GuideWindowRuntime/` 独立测试程序，直接链接生产窗口、协调器与会话模型，替换 CoreHost 和图文服务为受控内存服务。它创建实际 WinUI 窗口，完成测试后关闭退出；不会连接游戏、修改用户进度或访问网络。快捷键从原生协议事件入口注入，因此此处覆盖窗口逻辑，不代表全局键盘钩子已经在游戏中验证。

本机 Release 构建 0 警告、0 错误；实际执行 14 个场景全部通过、进程退出码 0。包含 F8 连续开关、查询/注册/图文加载期间关闭、不被迟到响应重新打开、成功保存关闭和失败保留、精确身份的完成快捷键、旧代次隔离、过期 WinUI 路线快照、查询期间完成重查、断开连接和无剩余目标。

另补上窗口注册等待期间的完成核对：云端同步或旧代次完成事件可以更新当前点为已完成，却不应关闭新会话；实际激活窗口前重新核对完成代次，变化时重新查询目标。真实窗口测试验证此时未加载/显示 A 的攻略，直接读取并显示 B。

实际窗口证据：`OverlappedPresenter.HasTitleBar=false`、`HasBorder=false`，显示/隐藏时同时核对 `AppWindow.IsVisible` 与原生 `IsWindowVisible`。记录 HWND=3146132、style=`0x144C0000`；完整 `WS_CAPTION` 掩码不存在（Windows 的 `WS_CAPTION` 是 `WS_BORDER | WS_DLGFRAME`，不能把单独残留的 `WS_DLGFRAME` 位误判成标题栏）。证据为 `out/guide-window-harness-build.log` 和 `out/guide-window-runtime/guide-window-tests.log`；复现步骤见该测试目录的 README。

## 尚未做的游戏内验收

尚未进行真实游戏的键盘钩子、窗口拖动、Z 保存后的焦点交接和图片放大期间关闭验收；离线通过不计作游戏内验收通过。新版已更新至常用 `x64/Release/IMao-WinUI.exe`，无需继续使用独立预览目录。本次检查启动的旧预览实例已关闭，未修改用户点位进度。
