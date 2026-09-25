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

## 交还前台前先等手柄松开（2026-09-25）

**实机反馈**：手柄模式下长按 A 收集完一个点，角色紧接着**闪避**一下。

**根因不在按键转发，而在交还前台的时机。** 本项目**不拦截手柄输入**（见本文下面那条注意），
游戏一直在读同一只手柄，只是它没有前台时不做响应。长按 A 的那 600 毫秒里前台在攻略窗口上，
游戏看不见 A；收集一完成：

1. `ApplyCompletion` 判定这次完成要收起攻略；
2. 攻略窗口隐藏 → 前台自然回到游戏（并伴随一次 `SetForegroundWindow`）；
3. 玩家的手指这时**还按在 A 上**，几十到两百毫秒后才松开。

游戏是第 2 步才拿回前台的：它没看见按下，却看见了松开，于是把那次松开当成一次**完整的 A**
（游戏里 A = 闪避）。**这正好是 `Docs/archive/GamepadAdaptationDesign_20260908.md` §6
「焦点与按住过渡」那条验收项要挡住的"遗留动作"，它一直没被验过。**

**修法**：把"什么时候把前台还给游戏"变成一条受控的等待——`Models/GuideFocusHandoff.cs`（纯函数）
+ `MarkerGuideCoordinator.DeferCloseUntilPadNeutral`：

- 手柄**任意键/扳机/摇杆**没有回中位时，先不收窗口、不交还前台；30 毫秒轮询一次，回中位立刻交还；
- 上限 **2 秒**：玩家一直按着（或手柄读不到）也照旧交还，绝不能把攻略窗口永远停在前台困住玩家；
- 手柄本来就是松开的（含没有读到手柄）→ 照旧立刻交还，不引入任何额外延迟；
- 读的是**输入服务同一个读取器、同一个设备号**（`ReadGamepadSample` / `GamepadDevice`），
  不会出现"两只手柄各读一个"的情况；
- 诊断：`guide-focus handoff-deferred buttons=… lt=… rt=…` 与 `handoff-now … neutral=True`。

> **2026-09-25 实机补报：长按 Y 跳过也一样。** 第一版只把这条等待加在**长按 A 完成**那条路上，
> 于是就出现了"跳过成功之后角色跳一下"——Y 既是触发这次跳过的键，也是游戏里的动作键（跳跃），
> 收窗口时它还按着。
> 已改成**通用规则**：`DeferCloseUntilPadNeutral(game, close)` 接收"稍后才做的收尾动作"，
> **每一个"攻略收起后前台回到游戏"的路径都走它**——长按 A 完成、长按 Y 跳过、大世界里
> LB+X 收起攻略、LB+B 完成附近点位。判断标准只有一条：**收起窗口的那一刻，手柄是不是回中位了**。

> **2026-09-25 实机第二次补报：长按 A 完成之后，手柄焦点跑到"谁都不是"的地方**，于是 LS 切不了
> 聚焦、B 也退不出攻略。根因是**前台交还被 `restore` 挡住了**：
> `restore = standaloneGamepadGeneration == generation && StandaloneForegroundOwner(window) != null`
> 只在"收窗口那一刻这份攻略正好占着前台"时为真，而前台交还写成 `if (restore) SetForegroundWindow(game)`。
> 前台不是攻略窗口时（延迟等待期间、玩家切过聚焦、大图刚收起……）**没有任何人把游戏放回前台**，
> 窗口一藏，系统只会挑"下一个"窗口——那未必是游戏。
> 修法：**前台必须有明确归宿**。只要这是手柄开出的攻略、游戏窗口还在，收起后就无条件调用
> `ReturnForegroundToGame(game, stage)`（它自己会在前台已经是游戏时跳过），不再看 `restore`；
> 延迟等待那一跳（`handoff-now`）也走同一条。诊断新增
> `guide-focus <stage> foreground-return accepted=… before=… game=… after=…`——
> 下次再出现"焦点不知道去哪了"，这一行能直接分辨是"没交还"还是"交还被拒"。
> **这条没有自动化用例**：本套件共享真实桌面前台，"把前台切给攻略窗口"在本会话里稳定失败
> （`guide-focus-toggle` 一次都不出现，模式停在 GuidePassive），硬写会假红；
> 手工复现步骤：手柄呼出攻略 → LS 把聚焦切到攻略窗口 → 长按 A 完成 → 立刻按 LS / B，看是否还能切换。

纯规则由 `Tests/ManagedRuntime/GuideFocusHandoffTests.cs` 钉住（按住 A/Y/X/B/LB+X、摇杆偏出、
扳机按下都要等；松开的立刻交还；死区内的小抖动不算按住；到上限必须放弃等待；取消后不再触发）。

**真实手柄上的手感**：长按 A 收集完不再闪避（2026-09-25 实机确认现象）、长按 Y 跳过完不再触发
游戏动作（本次修正的目标）。

## 手柄呼出攻略不再抢前台，LS 切换聚焦（2026-09-25）

**实机反馈**：手柄呼出攻略后窗口默认取得前台，玩家在游戏里的操作（移动等）随之失效——想看攻略就得停下手。
手柄按键本来就少，所以这里做"聚焦切换"，而不是再加一个"关闭攻略"的键。

**新行为**：

| 情形 | 现在怎么做 |
|---|---|
| 手柄开出的攻略（大世界 LB+X 的路线回退、大地图工具条「当前目标攻略」、「附近点位」选择列表里选中的那个点） | **窗口置顶显示但不激活**，前台交还游戏：可以继续用手柄玩，攻略就在旁边看 |
| 想操作攻略窗口 | 按 **LS（左摇杆按下）**：把前台切到攻略窗口，这时 A/B/翻页/长按 A 完成/长按 Y 跳过才作用于攻略 |
| 想接着玩 | 再按一次 **LS**，或按 **B**（见下）：把前台交还游戏，**攻略继续显示**（不关闭）。放大看图也会随之一并收起，免得单独悬在游戏上方 |
| 想关掉攻略 | **LB+X**：攻略开着时这条世界快捷键就是"收起"（托管层直接关，不绕原生那条依赖世界观测的路） |
| 聚焦在攻略窗口时按 B | **退出攻略聚焦、回到游戏**（与 LS 同义，攻略继续显示）。要收起整份攻略是 **LB+X**；关闭按钮也仍然收起它 |
| **X 放大图片** | 大图是独立窗口，前台随之转到大图窗口；这时手柄**仍然归攻略**（`Image` 模式，扳机缩放/摇杆平移/B 返回都在）。按 **B** 收起大图并**回到攻略窗口**（焦点自然衔接），再按 B 才是"退出聚焦回到游戏" |
| **LB+B 完成附近点位** | **两种聚焦状态都能用**：聚焦在游戏、或聚焦在攻略窗口上都可以。完成后如果核心回的是"攻略展示的那个点位已完成"，攻略窗口一并关闭 |
| 单独按一下 LB | **什么都不做**（大地图上的"打开工具台"只在大地图生效，不会弹在攻略上面闪一下） |
| 大地图工具台的 LB 入口 | 保持"只在大地图出现" |
| 从 RB 助手列表里打开的攻略 | 保持原有激活行为：那份窗口属于助手会话（列表里的 B 要能回到列表），是玩家主动进菜单浏览的路径 |
| 前台既不是游戏也不是攻略（玩家切去别的程序） | LS 与两条和弦都不做，不抢别人的焦点 |

> 2026-09-25 第二次修正：上一版按"源窗口是不是游戏窗口"区分，于是**大地图工具条那条路仍然抢前台**
> （实机日志里 `guide-focus opened-focused` × 5，39 次手柄呼出全部带 route，都是工具条入口）。
> 现在手柄开出的攻略一律被动；只有 RB 助手会话内打开的攻略保持激活。
>
> 2026-09-25 第三次修正：上一版在被动状态下放行 LB/RB，想用"同一个入口"把攻略收回去，
> 结果实机按 LB 弹出了**本该只在大地图出现的工具台**，而且一闪而过（它要求大地图/前台，
> 下一个 50 毫秒的定时器就把自己关了）。
>
> 2026-09-25 第四次修正（本次）：按实机要求定成最终键位——
> **单独按 LB 什么都不做**；**LB+X 开/关攻略**；**LB+B 完成附近点位**，且**两种聚焦状态都要能用**；
> LB+B 完成后若完成的就是攻略展示的点位，攻略窗口一起关闭。
> 后两条要求"聚焦在攻略窗口时"也能用，而原生那条世界快捷键原本要求游戏在前台，所以把
> "游戏前台 **或 我们自己的攻略窗口前台**"抽成 `WorldChordAllowed`，四处判定（关联应答入队、
> App 的 Take 与可见性、附近动作本身）统一改用它；别的程序在前台时两者都 false，和弦照旧不生效。
>
> 2026-09-25 第五次修正（本次）：实机反馈"**X 放大图片之后焦点没到大图窗口**，按 B 回不去，
> 也没法像还没定义聚焦切换时那样自然衔接"，同时要求"**聚焦在攻略窗口时 B 也能退出聚焦回游戏**"。
> 根因是"这份攻略是不是占着前台"只看攻略窗口自己：大图是**独立窗口**，打开时前台转到大图窗口，
> 于是这一刻被判成"聚焦在游戏上"（`GuidePassive`）——手柄整个让给游戏，B 返回、扳机缩放、
> 摇杆平移全部失效。现在判定改看**一对窗口**（`GuideWindowFocus` 纯函数），并顺手把 B 定成
> "退出聚焦"（与 LS 同义）：收起大图、退出聚焦都不再关掉整份攻略。

**实现要点**：

- "这段手柄输入归谁"只看"**这份攻略**（攻略窗口 **加上它打开的放大图片窗口**）是不是占着前台"
  （`MarkerGuideCoordinator.StandaloneForegroundOwner` → 纯函数 `Models/GuideWindowFocus.cs`）：
  是前台 → `Detail`/`Image`（攻略导航、完成、跳过全部启用）；不是前台 → 新增的
  `GamepadInputMode.GuidePassive`：这段输入留给游戏，只保留 LS（切换聚焦）和下面那两条世界和弦，
  单独按 LB 与其余按键（含 A/B/X/Y/RB）完全不碰。
- **两条世界和弦在攻略开着时也认**（`GuideWorldChordLatch`，与世界里的和弦同规则：LB 先按或同一帧
  齐按、补上 B/X 后松开生效、反向/补别的键/带扳机都作废）：**LB+B 完成附近点位**、
  **LB+X 收起这份攻略**。之所以必须在托管层认：攻略窗口自己在前台时游戏不在前台，原生收不到；
  而手柄归游戏（被动）时攻略又不解释手柄——两条路都指望不上。LB+X 在攻略开着时一定是"收起"，
  直接走托管层，不绕原生那条依赖世界观测的路。
- LS 的按下检测放在输入状态机**之外**（`GamepadInputService` 里的 `GuideFocusToggleLatch`）：
  被动模式下这段输入不走状态机，而这个切换也不该被"请先松开按键"之类的等待挡住。
  两条和弦同样只看原始样本；攻略一打开时三者都取一次基线，玩家已经按着的键不算一次。
- 攻略详情页里的 LB（上一张）不受影响：解释器遇到"LB+B"这种混合按键会自行取消，
  和弦由上面的闩锁单独认，两者不会互相吃掉。**B 在新键位下是"退出聚焦"**（不是关窗）：
  它由 `HandleGamepadAsync(Back)` 转成"把这份攻略占着的前台交还游戏"，攻略继续显示；
  助手会话（RB 列表）里打开的攻略保持原义（B 回到列表），窗口自己的关闭按钮走"收起这份攻略"
  那条路（`CloseStandaloneGuideAsync`，与 LB+X 同一条）。
- **唯一候选的"附近攻略"不再先建选择窗口**（实机反馈：手柄呼出时即便范围内只有一个点位，
  也会有一瞬间的选择窗口；键鼠 F8 没有这个现象）。根因是那条路先创建并**激活**选择窗口，
  再因为只有一个候选自动把它选掉。现在改用与键盘入口**完全相同的关联查询**
  （`markerGetNearbyGuide`）重新解析唯一身份后直接打开，解析不出唯一身份（多候选、或点位/位置
  已变化）才退回原来那条列表老路。诊断：`nearby-single-direct` / `nearby-single-unresolved`。
- 打开时不激活窗口，并把前台交还游戏：工具条那条路由它自己的交接归还，选择列表那条路
  由 `ReturnFocusToGameAsync` 归还（否则窗口一关，玩家会被留在一个已经不存在的菜单上）。
- 关闭用同一入口：原生在"这次是攻略意图 + 手柄 + 已有一份同档案的可见攻略窗口"时不再查附近点位，
  而是补发一次 `markerGuideShortcut`（`reason=guide-visible-toggle`），托管层据此关闭。
  托管层同时放宽了那两道"前台必须在游戏/源窗口上"与"需要交接租约"的门槛——关窗不改前台归属。
- 窗口侧：`ShowMarkerAsync(activate: false)` 用 `SW_SHOWNOACTIVATE` 显示（`HideGuide` 走的是
  `AppWindow.Hide`，不显式显示会停在隐藏状态）；手柄提示常驻「LS / B 退出聚焦回到游戏 · LB+X 收起攻略」。
- **大图窗口的前台归属**：`GuideWindowFocus.Owner(前台, 游戏, 攻略窗口, 攻略可见, 大图窗口, 大图可见)`
  返回 `Other/Game/Guide/GuidePicture`，`GuideOwns` 只认后两者。手柄上下文、LS/B 的聚焦切换、
  交还前台、诊断日志全部只问它一处。大图**没开着**时它的句柄一律不算数（句柄可能被系统复用），
  攻略窗口不可见时也不认它。
- **从大图返回 vs 交还前台是两条路**：关大图时窗口记下"关之前它是不是当时的前台窗口"
  （`GuideImageStage.ReturnToGuide` / `LeaveForeground`）。玩家按 B/Enter/ESC/关闭按钮 → 前台回到
  攻略窗口；而 LS/B 退出聚焦、LB+X 收起攻略是**先把前台交还游戏、再收起大图**
  （先收会让"从哪个窗口交还"的前提当场失效），这时前台必须留在游戏上、不能再被 `Activate()` 抢回攻略窗口。
- **LS 闩锁在派遣期间也要跟着按键走**：聚焦切换是异步派遣，派遣期间输入服务原本整段 `return`，
  于是"松开 LS"那一帧没人看见，闩锁停在"还按着"，玩家下一次按 LS 被当成同一次按住而失效
  （实机与用例里连按两次 LS 只生效一次）。现在边沿记录在派遣判定**之前**，真正发号仍等派遣空闲。

**注意**：LS 是单击语义，而这个键同样会送到游戏——如果游戏里 L3 绑定了动作（例如疾跑/蹲下），
按 LS 切聚焦时游戏也会响应（本项目不拦截手柄输入）。若实机觉得冲突，可改成"按住 LS"或"LS+RB"
等组合，边沿语义与用例在 `Tests/ManagedRuntime/GuideFocusLatchTests.cs`。键鼠那条线不受影响：
Z 与 G 只要"攻略可见 + 游戏或攻略窗口在前台"就生效，不需要先点窗口。

**证据**：`GuideWindowRuntime.exe --test-route-controller`（11 例全过）里的
「standalone gamepad guide keeps the game focused and LS toggles focus」、
「X enlarges the picture and the pad stays with the guide until B returns to it」、
「a single nearby candidate opens its guide without ever building the chooser」、
「the same gamepad shortcut closes the guide it opened」、
「LB alone is idle, LB+B completes and LB+X dismisses」、
「the route target guide only opens while the route is guiding」、
「toolbar handoff shows the guide passively and its own entry closes it again」在真实窗口上验证：
呼出后前台仍在游戏、`GuidePassive` 不吃 A 键；唯一候选直接开攻略，且**选择窗口从未被创建或激活**
（没有 `markerBindNearbyCandidates`、没有 `choices-activation`）——把这条修复临时关掉，这条用例
就失败在"没有建选择窗口"那一句上；LS 切到攻略窗口后翻页生效；再按 LS 前台回到游戏且攻略仍然可见；
**B 在攻略窗口上是"退出聚焦"**（攻略继续显示）、被动状态下 B 完全到不了攻略；
**X 放大后前台转到大图窗口，模式仍然是 `Image`**（核心同步登记大图句柄用于攻略快捷键）、
**B 从大图回到攻略窗口**、再按 B 退出聚焦回游戏、最后 LB+X 收起攻略；
单独按 LB 什么都不做也不弹工具台；LB+B 在**两种聚焦状态**下都会请核心完成附近点位，且不会顺手翻页
或关闭攻略；核心回"攻略展示的点位已完成"时攻略窗口一并关闭，回别的点位时保持打开；LB+X 收起攻略；
路线暂停时按同一个快捷键只提示一句、不打开攻略，改成指引中再按就正常打开。

**反向证明（用例真的抓得住旧行为）**：把"这份攻略是不是占着前台"改回只看攻略窗口自己
（`!IsForeground(direct)`），三条用例分别失败在
`the pad still belongs to the guide while the enlarged picture is in front`、
`B release hands the pad back to the game, exactly like LS`、
`B on the selected candidate's guide hands the pad back to the game`；
把 `RootKeyDown` 里的图片路由关掉，键鼠那条用例失败在 `the guide window consumes Enter`。

另有纯模型用例 `Tests/ManagedRuntime/GuideFocusLatchTests.cs`（LS 与两条和弦的边沿/组合规则）、
`GuidePictureAndFocusTests.cs`（Enter/ESC 路由与"一对窗口"的前台归属，含隐藏大图句柄/已关闭攻略
的过期句柄不算数）与 `RoutePlanningState.IsGuiding` 的用例。

## 键鼠：Enter 放大图片，Enter / ESC 退出大图（2026-09-25）

**实机反馈**：键鼠模式下呼出攻略窗口后**没有任何键能放大攻略图片**（只有"放大图片"按钮）；并明确要求
Enter 同时作为"放大"与"退出大图"的快捷键，**ESC 也退出大图且是默认生效、不可修改的按键**。

**规则**（纯函数 `Models/GuidePictureKeys.cs`，被两个窗口共用）：

| 按键 | 攻略窗口（大图没开） | 攻略窗口（大图开着） | 大图窗口 |
|---|---|---|---|
| Enter | 打开大图（当前这张图真能放大时） | 收起大图 | 收起大图、回到攻略窗口 |
| ESC | 什么都不做（它不负责关整份攻略） | 收起大图 | 收起大图、回到攻略窗口 |

- Enter/ESC 都是**固定键**：`RuntimeConfiguration.IsSupportedHotkey` 只接受字母、数字、F1–F12、
  PageUp/PageDown，所以玩家不可能把它们分配给别的功能，也就不会有冲突（用例钉住这一点）。
- **这两个键和 Z/G 一样不受"攻略窗口是不是前台"限制**：F8 打开的攻略窗口在生产里常常拿不到键盘焦点
  （实机日志里连按 F8 的 `guide-shortcut action=close-visible-guide foreground=<game>` 就是证据），
  只在窗口里监听 `PreviewKeyDown` 是不够的。原生钩子按纯函数
  `GuideHotkeyRouting.h` 的 `GuidePictureKeyOwned` 判定归属（攻略可见 + 游戏或攻略在前台），
  再把 `{"type":"markerGuidePictureKey","key":13|27}` 转交托管层执行一次并吞掉这个键（与跳过键同一套路）；
  其中 **ESC 只有在大图真的开着时才归攻略**——否则会把大地图的"取消手势/回到平移"吃掉。
  原生侧据此需要知道"现在登记的是不是大图窗口"，所以 `markerSetGuideWindow` 的载荷多了一个
  `picture: true` 标记（`MarkerGuideProtocol::Registration` 只接受 true，缺省即攻略正文）。
  诊断：`guide-picture-key`（`key/imageOpen/pictureAvailable/fg`）。
- 窗口自己收到按键时走同一条规则（`MarkerGuideWindow.PressKey` / `GuideImageWindow.PressKey`），
  所以没有核心钩子的环境（测试夹具、核心未启动）行为一致。
- 大图窗口收起时前台回到攻略窗口（`GuideImageStage.ReturnToGuide` → `back.Activate()`），
  所以键鼠这条线上 Enter/ESC 的进出也是自然衔接的。
- 「放大图片」按钮的悬停提示写着「放大图片（Enter）」；使用指南里同步说明 Enter/ESC 与手柄的 X/B。

**证据**：`GuideWindowRuntime.exe`（真实窗口）新增用例
「Enter enlarges the guide picture and Enter or Esc closes the big picture」验证：Enter 真的开出
独立大图窗口并拿到键盘焦点、ESC 收起它并把焦点交回攻略窗口、Enter 在大图里同样收起（同一个键两个方向）、
无关按键不被消费、大图开着时翻页失败会在**大图窗口**里报错且标题跟着页码走，
以及**钩子转交那条路**（`markerGuidePictureKey`）能开出/收起同一份大图、别的档案的转交不生效。
这条用例同时取代了原先断言已被删除的 `MarkerGuideDialog`/`imageDialog` 字段、长期失败的旧用例。

**反向证明**：把 `RootKeyDown` 里的图片路由关掉，这条用例失败在 `the guide window consumes Enter`。
原生侧的归属规则由 `IMaoRoutePlanningTests.exe` 的 `GuideHotkeyRoutingTests` 钉住（Enter 在"游戏前台"
时归攻略、ESC 没有大图时不归攻略、没有攻略窗口时不归攻略、别的程序在前台时不归攻略），
`MarkerGuideProtocolTests` 钉住 `picture` 标记只接受 true。

## 尚未做的游戏内验收

尚未进行真实游戏的键盘钩子（含本次新增的 Enter/ESC 转交）、窗口拖动、Z 保存后的焦点交接和图片放大期间关闭验收；
离线通过不计作游戏内验收通过。**本次要看的四件事**：
1. 键鼠 F8 呼出攻略后 **Enter** 能放大图片，再按 Enter 或 **ESC** 能退出大图（游戏在前台时也该生效，走钩子转交）；
2. 手柄在攻略窗口按 **X** 放大后，扳机缩放/摇杆平移/B 返回都正常，**B 回到攻略窗口**；
3. 聚焦在攻略窗口时按 **B** = 退出聚焦回到游戏（攻略继续显示），**LB+X** 才是收起攻略；
4. 四条诊断在 `%LOCALAPPDATA%\IMao-WinUI\Logs\gamepad-*.jsonl`：
   `guide-picture-key`（键鼠图片键）、`guide-focus`（聚焦切换/交还）、`guide-shortcut`、`guide-world-chord`。

新版已更新至常用 `x64\Release`（并同步 `out\map-test`，四件产物 SHA-256 相同、`sourceCommit=46e6da4`、
`sourceDirty=false`），无需继续使用独立预览目录。本次检查启动的旧预览实例已关闭，未修改用户点位进度。
