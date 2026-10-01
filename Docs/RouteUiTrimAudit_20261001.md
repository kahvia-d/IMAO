# 路线功能界面精简 · 审阅（哪些只能在游戏内用、哪些放 UI 上没有意义）

日期：2026-10-01
分支：`feature/imao-ui-streamline`
状态：**只审阅，未改任何一行代码**（本轮交付物就是这份清单）。

## 0. 一句话结论

桌面端「路线」页（`FunctionPage.xaml`）现在把**游戏内大地图工具窗**（`MapToolsWindow.RenderRoute`）的操作几乎原样抄了一遍，
但其中**约一半按钮在 UI 上点了必然报错或永远无效**——因为它们的判据是"大地图已打开并完成识别"，而这个条件只能由游戏提供。
UI 真正该做的是**介绍、配置、查看与离线可用的少数操作**；"选点/画线/生成/开始指引"这一串的真实工作面在游戏内工具栏。

> ⚠️ 还有一个更根本的事实：**桌面端根本没有打开那个工具窗的入口**——游戏内工具台只在大地图上开。
> 而且**它不是手柄专属**（见 §1.1）：键鼠点大地图底部中央的圆钮，手柄按 LB，两条路都进同一个窗口。
> 所以现在**不存在"UI 与工具栏功能重复"的冗余**，只存在"UI 摆了很多按不动的门"。
> 精简掉它们**不会丢任何现有能力**。
>
> 写玩家可见的文案时不要默认手柄：这个工具的输入方式一直是键鼠与手柄并列的。

---

## 1. 先把"界面"分清楚：三个面，不是一个

| 面 | 实体 | 入口 | 现状 |
|---|---|---|---|
| **A. 桌面主窗口「路线」页** | `IMao-WinUI/Views/FunctionPage.xaml`（4 张卡 + 手绘折叠区） | 侧栏「路线」；概览页「前往路线」；帮助页「前往路线」 | **活的** |
| **B. 游戏内大地图工具窗**（玩家口中的「路线工具栏」= 面板「路径自动规划」） | `IMao-WinUI/Views/MapToolsWindow.cs` 的 `RenderRoute`（`:199-255`）与 `RenderRoutes`（`:261-353`） | **键鼠：点大地图底部中央的圆钮**；**手柄：按 LB**。两条路都走 `markerMapToolsRequested` → `MapToolsController.OpenAsync` | **活的，且是真正的操作面** |
| **C. 原生 ImGui 那套工具栏** | `IMao-Core/src/ImguiDraw/Items/DrawMarkerInteraction.cpp` 的 `BuildPlanningToolbar` / `PlanningPanel` / `DrawPlanningToolbar`（`:656-800+`） | 无 | **死代码**，函数上方 `:650-655` 的注释已经写明"没有调用点" |

### 1.1 B 的输入方式（两个都成立，不要只写手柄）

| 路径 | 证据 |
|---|---|
| **键鼠**：大地图底部中央的圆钮（深色圆底 + 青色描边 + 三条滑块线） | `DrawMarkerInteraction.cpp:1267-1294` 的 `DrawMapToolsLauncher` 画它并登记命中区 `maptools:open`（`:1291`）；`:269-270` 的命中是真正的圆形判定；`:541-544` 左键按下即捕获；`:1184-1191` 在 `running && bigMap && observable && 游戏前台` 时发布 `markerMapToolsRequested`。**这条路径与手柄无关** |
| **手柄**：大地图按 LB | `IMao-WinUI/Models/GamepadInput.cs:284`（LB 松开 → `GamepadAction.OpenToolbar`）→ `GamepadInputService.cs:372-376` → 同一个 `MapToolsController.OpenAsync` |
| 窗口内的画布手势键鼠同样可用 | `DrawMarkerInteraction.cpp:492` 的 `focused = gameFocused || ToolsCanvasFocused()`，`:141-146` 的 `ToolsCanvasFocused()` 就是"工具窗持有焦点且画布就绪"；`:552-554` 鼠标松开提交框选/套索；`:478` `Shift + 左键` = 临时矩形框选 |

> 这正是本页 01 卡最初写错的地方：我按 `OpenAsync` 的调用点得出"唯一入口是 LB"，漏了**圆钮这一条完全不经过手柄**的路。文案已按"键鼠优先、手柄并列"改写。

补充事实：

- B 是**真正的 WinUI 窗口**（`Window`），有原生登记会话（`markerMapToolsRegister` / `markerMapToolsInput` / `markerMapToolsDismissRequested`），
  手柄样本由 C# 转发给原生做光标/框选/套索几何（`MapToolsController.cs:313-340`）。
  即：**玩家看到的是 WinUI 按钮，手势几何在 C++**。
- C 死掉以后，`RouteGamepadButtons()`（`DrawMarkerInteraction.cpp:953-959`）**永远返回空**（`route:` 前缀的命中区只由 `BuildPlanningToolbar` 产生），
  于是 C++ 里的 `ProcessRouteGamepad`（`:963-...`）和 `RouteGamepadController` + `RouteGamepadInputHost`（C# 侧旧的透明输入窗口方案）
  在生产里都走不到有效路径——`GamepadInputService.cs:372-376` 只在 `mapTools is null` 时才回退到它。
  ⟹ 这两块是**第二次精简的候选**（本轮不动，见 §7）。
- B 的页面白名单在原生侧也有一份：`IMao-Core/src/Runtime/MapToolsBridge.h:104`（`home`/`route`/`filter`/`routes`）。改页名要两处同改。

---

## 2. 路线命令的完整工作面（17 组 action）

来源：`IMao-Core/src/Runtime/RoutePlanningService.cpp` 的 `RoutePlanningService::Command`（`action` 分支 `:871-998`，拒绝点 `:998`）。

| action | 语义 | A 桌面 UI | B 游戏内工具栏 | 核心侧真实门槛（代码） |
|---|---|---|---|---|
| `new` | 进入选点 | 「开始选点」`:12` | 「开始选点 / 继续选点」`:218` | 无门槛；但**草稿要等大地图被观测才建立**（`:716-717`） |
| `end` | 退出选点（草稿保留） | 「结束选点」`:13` | 「退出选点」`:214` | 无 |
| `tool` | `pan/point/box/lasso/start` | 「移动地图/矩形框选/自由套索/指定起点」`:19` | 同 5 个 `:211` | **`DraftLocked()` → `Scene::IsKnown(scene)`**，否则 `请先打开大地图并完成识别`（`:120`） |
| `addVisible` | 加入当前视野 | 「加入可见点」`:14` | 「加入当前视野」`:212` | 用 `r.visible`（**只有大地图在观测时才非空**，`:708-715`） |
| `add` / `remove` / `toggle` | 增/删点位 | 列表「移除」`:25` | 由画布手势产生 | 同 `tool`：`ResolveLocked(r.scene,key)`（`:387`） |
| `undo` / `clear` | 撤销 / 清空 | `:15` `:16` | `:212` | 同 `tool`（`DraftLocked`） |
| `setStart` | 指定起点 | 仅 `tool:start` 一个按钮 | 「指定起点」+ 地图点击 | 要"当前场景"（`:880`） |
| `generate` | 生成预览 | 「生成路线预览」`:35` | 「生成预览」`:213` | `QueueSolveLocked`：**起点有效**（`draft.start` / `mapStart` / `player`）+ ≥1 个未完成目标（`:394-409`） |
| `replan` | 重新规划剩余目标 | 「重新规划」`:46` | 「重新规划」`:230` | 同上，且活动路线场景 == 当前场景（`:397`） |
| `activate` | 开始导航 | 「开始导航」`:35` | 「开始指引」`:214` | 必须有预览（`:900`） |
| `pause` / `resume` / `stop` | 暂停 / 继续 / 退出导航 | `:46` | `:223` `:230` | 需活动路线 |
| `skip` / `undoSkip` | 跳过 / 撤销跳过 | `:45` `:46` | `:229` | 需活动路线 + 当前目标 |
| `farm` | 刷怪采集开关 | ❌ 没有 | 「刷怪采集：开/关」`:235` | 需活动路线；**开关写在路线文件里**（`:938-951`） |
| `autoReplan`（走 `ConfigureAsync`，不是 action） | 实时规划开关 | 「实时规划剩余路线」`:47` | 「实时规划：开/关」`:236` | 设置项 |
| `guide` | 当前目标攻略 | 「查看当前目标攻略」`:45` | 「当前目标攻略」`:228` | 需 `Guiding` + 当前目标（`MapToolsWindow.cs:225-228` 的注释解释了为什么不能只看有没有目标） |
| `complete` | 完成当前目标 | 「完成当前目标」`:45` | 由攻略页按住 X / 小地图快捷键做 | **不做距离校验**，直接写完成（`:960-966`） |
| `save` / `load` / `delete` | 保存 / 载入 / 删除 | 「保存预览路线/保存活动路线/载入/删除」`:57-59` | 列表页「保存这条路线/删除这条路线」`:286-287` | 无地图门槛 |
| `switch` + 按路线借筛选 | 应用某条路线并开始指引 + 只显示它用到的类型 | ❌ 没有（UI 只有 `load`） | 列表页点一行 `:377-382` → `MapToolsController.cs:433-451` | 无地图门槛；借出的筛选退出导航自动归还（`RouteFilterPlan`/`RouteFilterSnapshot`） |
| `list` / `state` / `current` | 读状态与列表 | 有 | 有 | 无 |
| `handStart/Point/Finish/Cancel/Commit/Discard/Undo` | 手绘 | 折叠区 4 个按钮 `:70-73` | 列表页「开始/结束/保存/放弃手绘」`:338-350` | **`请先在游戏大地图上打开要绘制的区域`**（`:169`） |

---

## 3. 判据一：这些功能**离开游戏就没有对象**（UI 上点了必然报错或永远无效）

| # | UI 元素 | 位置 | 证据 | 在 UI 上点击的结果 |
|---|---|---|---|---|
| 1 | 「矩形框选」`tool:box` | `FunctionPage.xaml:19` | `tool` 分支本身无门槛（`:876-878`），但草稿要等画布手势写进去 → `DraftLocked()`（`:120`） | 命令**被接受**（界面不报错），但游戏里没有画布可框选，**什么都不会发生** |
| 2 | 「自由套索」`tool:lasso` | `:19` | 同 1 | 同 1 |
| 3 | 「指定起点」`tool:start` | `:19` | 同 1；真正的起点写入走 `setStart`，要求"当前地图"（`:880`） | 同 1（起点只能在游戏里点地图指定） |
| 4 | 「移动地图」`tool:pan` | `:19` | 同 1 | 同 1（UI 上"移动地图"没有可移动的对象） |
| 5 | 「加入可见点」`addVisible` | `:14` | `:886` 用 `r.visible`；`r.visible` 只在 `ObserveMap` 里填（`:708-715`），`MapUnavailable` 会清空（`:734-740`） | 界面上不报错，提示行显示 `已追加 0 个目标`——**看着成功，其实什么都没加** |
| 6 | 「结束选点」`end` | `:13` | `:875` 无门槛 | 真的能生效，但它结束的是"选点模式"——**而选点模式只能在游戏里产生内容** |
| 7 | 手绘折叠区 4 个按钮 | `:70-73` | `RoutePlanningService.cpp:169` | `请先在游戏大地图上打开要绘制的区域`（InfoBar 报错）；只有 `handCancel`/`handDiscard` 这类清理动作不报错 |

> 报错怎么到界面上的：`FunctionPage.xaml.cs:148-155` 捕获异常 → `AutoRouteMessage` InfoBar；
> 命令"被接受但没做事"则通过 `routePlanningChanged` 推送的 `message` 显示（`CoreHostService.cs:566-569`）。
>
> 关键细节：`new`（「开始选点」）在 UI 上**不报错**，因为它只写一个 `pendingNew` 标志，真正建草稿发生在下次 `ObserveMap`（`:716-717`）。
> 也就是说 UI 上按下去**界面上立刻显示"已进入选点"，但游戏里什么都没发生**——这是比报错更坏的反馈。

---

## 4. 判据二：这些入口和游戏内**重复，且在 UI 上信息更少 / 是死代码**

| # | UI 元素 | 位置 | 为什么在 UI 上没意义 |
|---|---|---|---|
| 8 | 路线名称 `TextBox` | `FunctionPage.xaml:56`；只被 `AutoRouteSave_Click`（`:186-194`）读 | 路线重命名已经搬到游戏内**路线列表页**（`MapToolsWindow.cs:286-287`「保存这条路线」用 `current.Name`）。该输入框在 UI 上既没接 `switch`/`hand*` 流程，也看不到已有名字，**基本是死控件** |
| 9 | 「手绘路线」折叠区（说明 + 4 按钮） | `:64-76` | 手绘的**加点**发生在游戏大地图（点击/快捷键），**保存/放弃**已经有专门的列表页（`RenderRoutes` 的手绘段 `:324-351`）。UI 这份是同一功能的**第二份说明 + 第二套入口**，而且都点不动 |
| 10 | 「保存预览路线」/「保存活动路线」两个按钮 | `:57` | 列表页对"预览"与"活动"给了**同一个按钮**（`saveCurrent`，`MapToolsController.cs:405-417` 自己分流）。UI 要玩家先分清预览/活动，是**把内部概念暴露给玩家** |
| 11 | 「已保存的路线」`ComboBox` + 「载入路线」 | `:58-59` | ① 只有名字/地图，**没有点数、来源（自动/手绘）、点位类型徽标**（列表页有，`MapToolsWindow.cs:361-384`）；② 载入后**不会像列表页那样按路线借出筛选**（`switch` 才借，`load` 不借）——同一个"换条路线走"，UI 的效果比游戏内**少一半** |

---

## 5. 判据三：UI 上**确实有意义**（不要一起砍掉）

| UI 元素 | 位置 | 为什么留 |
|---|---|---|
| 「完成当前目标」 | `FunctionPage.xaml:45` | 核心 `complete` **不做距离判断**（`RoutePlanningService.cpp:960-966` 直接写完成），所以"我已经自己搞定了"这类操作在 UI 上是**真能生效**的，而且不必回到游戏 |
| 「暂停 / 继续 / 跳过 / 撤销跳过 / 重新规划」 | `:46` | 都要活动路线，UI 上操作合法；回到游戏就是新状态 |
| 「停止导航」 | `:46` | 它是唯一能在游戏外结束导航的地方。⚠️ 但**"按路线借出的筛选"的归还逻辑只挂在游戏内那条路径上**（`EndRouteFilterLoanIfNavigationEnded` 只在 `MapToolsController` 的 `:425` `:511` 被调用），UI 上按 `stop` 不会触发它——筛选会停在"只剩这条路线用到的类型"的状态，直到下次开一次工具窗才被认领归还（`MapToolsController.cs:58-66`）。**这是"UI 少做了游戏内会做的事"的实例，属于要单独修的问题，不是"没必要"** |
| 「查看当前目标攻略」 | `:45` | 只是打开攻略窗口（`DrawItemBase::SelectMarker`），门口条件是"指引中 + 有目标"，UI 上能用 |
| 路线列表（只读那份） | `:23-27` `:36` `:50` | 「查看已选点位 / 预览顺序 / 活动路线」是**纯展示**，正好补上游戏内列表页看不见的序列与楼层/ID 细节 |
| 「实时规划剩余路线」开关 | `:47` | 设置页也有（`SettingsPage.xaml:118`），但它是**跨会话设置**，在路线页顺手切换是合理的（"三处重复"见 §6） |
| 概览页「本次路线」 | `StartPage.xaml:18-27` | 纯状态展示，好的 landing |
| 设置页：手绘快捷键、`刷怪采集自动标记范围`、路线文件目录 | `SettingsPage.xaml:142` `:167` `:225` | **配置**天然属于桌面端，游戏内没有键盘输入的地方 |

---

## 6. 结论：建议的精简清单（供下一步决策，本轮不实施）

### 6.1 删除（UI 上无对象）
`FunctionPage.xaml` 中：

- `:19` 整行 `WrapPanel`：「移动地图」「矩形框选」「自由套索」「指定起点」（4 个按钮）
- `:14` 「加入可见点」
- `:13` 「结束选点」
- `:64-76` 整个「手绘路线」`Expander`

→ 删掉后在原地换成**一句说明 + 一个指路**：「选点、框选、套索、指定起点、手绘都在**游戏内大地图的『路径自动规划』面板**里；按 `LB` 打开。」（帮助页 `UsageGuidePage.xaml:27-33` 已有对应文字，可以直接复用/加链接。）

### 6.2 改造（保留但改语义）
- `:12` 「开始选点」（`new`）：不要让它在桌面端"假装成功"。改成**状态说明**（"选点未开始 · 请在大地图工具栏点『开始选点』"），或调成只读提示。
- `:57-59` 「路线收藏」卡：删除名称输入框、删除「保存预览/保存活动」两键、删除「载入路线/刷新列表」；
  只留「已保存的路线」**只读列表**（名称 · 点数 · 来源 · 类型徽标 · 当前是哪条）+ 一句「切到哪条、开始/删除，都在游戏内『路线列表』里」。

### 6.3 合并（三处重复）
「实时规划」出现在三处：`SettingsPage.xaml:118`（设置）、`FunctionPage.xaml:47`（路线页开关 + `:48` 状态）、`MapToolsWindow.cs:236`（工具栏）。
建议**保留设置页 = 定义 + 游戏内工具栏 = 操作**，路线页降级为一行状态文字（`state.AutoReplanLabel`，`:48` 已经就是这个文本）。
理由：它本来就是个持久设置，不是"这一次路线"的东西。

### 6.4 补一个现在缺的入口（重要）
桌面端目前**没有**通向游戏内工具窗的任何说明入口。精简的同时应在概览页/路线页补一块**只读引导**：
「大地图上按 `LB` 打开『地图工具台』→『路径自动规划』；`RB` 是点位助手」+ 一张工具栏截图。
这正好对应你说的"UI 只需要对这些功能进行介绍"。参考现有截图：`Docs/images/readme/map-plan.jpg`。

### 6.5 不要动
- `complete` / `pause` / `resume` / `skip` / `undoSkip` / `replan` / `stop` / `guide`（§5）
- 三个「查看…」列表（§5）
- 设置页的快捷键与范围（§5）

---

## 7. 顺带发现（本轮不改，但值得记一笔）

1. **`RouteGamepadController` + `RouteGamepadInputHost` + C++ `ProcessRouteGamepad` 全链是第二套已退役的工具栏输入**（§1）。
   生产里 `GamepadInputService.cs:372-376` 只在 `mapTools is null` 时才会走到它，而 `App.xaml.cs` 注入的是真实 `MapToolsController`。
   它们现在只被 `Tests/GuideWindowRuntime` 使用。**如果要继续精简，这是下一个候选**，但它有一套真实窗口用例钉着，动之前要先决定那些用例怎么办。
2. **文档索引过期**：`Docs/NativeRoutePlanningMap_20260928.md:117` `:124` `:125` 写的行号（`FunctionPage.xaml.cs:221-259`、`MapToolsWindow.cs:173/184-211`）
   对不上今天的文件（今天分别是 `:162-235`、`:199`、`:207-238`）。改 UI 时顺手修掉，否则下一个人会照着错行号改。
3. `MapToolsBridge.h:104` 的页名白名单与 `MapToolsWindow.ShowPage`（`:149-167`）必须同步改——这是 `RouteLibraryAndHandDrawn_20260928.md` §7 R5 记过的坑。
4. 本文所有"点了会报什么"都是**读码推断**（错误文案直接取自源码字符串），**没有跑过程序**；真机确认时重点看 §3 的 7 项是不是真的报错/静默无效。

---

## 8. 证据索引（本文依据的代码位置）

| 主题 | 位置 |
|---|---|
| UI 路线页结构 | `IMao-WinUI/Views/FunctionPage.xaml:5-76` |
| UI 命令发送与渲染 | `IMao-WinUI/Views/FunctionPage.xaml.cs:101-235` |
| 游戏内工具栏按钮组 | `IMao-WinUI/Views/MapToolsWindow.cs:199-255` |
| 游戏内路线列表页 | `IMao-WinUI/Views/MapToolsWindow.cs:261-353` |
| 工具栏命令分发 | `IMao-WinUI/Services/MapToolsController.cs:364-539` |
| 借出/归还筛选 | `IMao-WinUI/Services/MapToolsController.cs:551-609` |
| 唯一打开入口 | `IMao-WinUI/Services/GamepadInputService.cs:372-376`；`IMao-WinUI/Models/GamepadInput.cs:284` |
| 状态模型（UI 可见字段） | `IMao-WinUI/Models/RoutePlanningState.cs:6-186` |
| 原生 ImGui 工具栏 = 死代码 | `IMao-Core/src/ImguiDraw/Items/DrawMarkerInteraction.cpp:650-655` |
| 命令全表与门槛 | `IMao-Core/src/Runtime/RoutePlanningService.cpp:853-1009` |
| 选点门槛 | 同文件 `:120`（`DraftLocked`）、`:383-393`（`AddLocked`）、`:394-413`（`QueueSolveLocked`） |
| 大地图观测/离开 | 同文件 `:708-725`（`ObserveMap`）、`:734-741`（`MapUnavailable`） |
| 手绘门槛 | 同文件 `:167-171` |
| 完成/攻略 | 同文件 `:952-966` |
| 玩家侧帮助文案 | `IMao-WinUI/Views/UsageGuidePage.xaml:12-36` |

---

## 9. 实施记录（2026-10-01，分支 `feature/imao-ui-streamline`）

### 9.1 落地的改动

只动了两个文件，`FunctionPage.xaml` 79 → 55 行、`FunctionPage.xaml.cs` 240 → 214 行：

| 文件 | 改了什么 |
|---|---|
| `IMao-WinUI/Views/FunctionPage.xaml` | 删：5 个工具按钮（移动地图/矩形框选/自由套索/指定起点）、加入可见点、结束选点、整块「手绘路线」折叠区、路线名称输入框、保存预览/保存活动两个按钮、已保存路线下拉 + 载入路线。改：页面结构变成 4 张卡（01 在哪里操作 = 只读说明；02 当前导航；03 路线预览；04 已保存的路线）；已保存路线换成带「点数 · 地图 · 来源」与类型徽标的只读 `ListView`；最小尺寸下不再是 17 个按钮挤在一起 |
| `IMao-WinUI/Views/FunctionPage.xaml.cs` | 删掉随之失效的 `AutoRouteTool_Click` / `AutoRouteSave_Click` / `AutoRouteLoad_Click` 与三处渲染；`RenderRouteState` 不再引用已删除的控件；实时规划开关的摆正改为按**核心快照**（`state.AutoReplanEnabled`）而不是本地 `Configuration`——设置页和游戏内工具栏都能改这一项，快照才是权威值 |
| `Tests/RoutePageRuntime/`（新增） | 新增独立验收程序：**链接生产那一份 FunctionPage**（XAML + 代码后置），只把 `CoreHostService` / `App` / `INavigationService` 换成夹具。不需要游戏、不需要原生核心，23 条断言 + 3 张截图 |

### 9.2 按 §6 清单的逐条对照

| §6 建议 | 本轮 |
|---|---|
| 6.1 删工具按钮、加入可见点、结束选点、手绘折叠区 | ✅ 已删 |
| 6.2 改造「开始选点」（不要假装成功） | ✅ 已删按钮，改为 01 卡的说明文字（连"假成功"的可能一起去掉） |
| 6.2 改造「路线收藏」卡（去名称框、去保存/载入） | ✅ 名称框、保存预览/保存活动、载入、下拉全部删除；只留只读列表 + 「刷新列表」「删除所选路线」 |
| 6.3 合并三处「实时规划」重复 | ⏸ **本轮没做**：`Tests/MainWindowRuntime` 里有一条用例专门钉「设置页改 → 路线页开关跟随 → 保存被拒时回滚」，删了会连带改旧夹具。留作下一轮，见 9.4 |
| 6.4 补一个游戏内工具窗的说明入口 | ✅ 01 卡就是它（LB / RB 的分工、哪些操作在那边、这一页留下什么） |
| 6.5 保留有意义的操作与只读列表 | ✅ `complete`/`skip`/`guide`/`pause`/`undoSkip`/`stop`/`generate`/`activate` 与三个「查看…」都留着 |

### 9.3 验证（本轮实际跑过的）

![精简后的路线页：无活动路线](images/route-ui/route-empty.png)

![精简后的路线页：活动路线 + 已保存的路线](images/route-ui/route-active.png)

（上面两张是夹具用生产的 `FunctionPage` 直接渲染出来的：`route-empty.png` 是空状态，`route-active.png` 带活动路线与 3 条保存路线；另有 800×500 最小窗口的 `route-800.png`。截图与日志同时保留在 `out/route-page-runtime/`。）

| 项 | 结果 |
|---|---|
| 生产构建 `IMao-WinUI.csproj` Release/x64 | **已成功生成，0 错误**（12 条既有可空性告警，与本次无关） |
| `Tests/RoutePageRuntime`（新夹具） | **23 条断言全绿**，进程退出码 0；日志 `out/route-page-runtime/route-page-tests.log` |
| 截图 | `out/route-page-runtime/route-empty.png`、`route-active.png`（含活动路线 + 3 条保存路线）、`route-800.png` |
| 命令身份仍然正确 | 夹具断言：`complete` 带点位 key + routeId + profileId；`stop` 带 routeId + profileId |
| 游戏内入口确实消失 | 夹具逐条断言 14 个被删标签在可视化树里都找不到 |

### 9.4 遗留 / 下一轮

1. **`Tests/MainWindowRuntime` 从 2026-09-27 起就编译不过**（不是本轮造成）：它的 csproj 只链接部分生产源文件，
   而 `SettingsPage` / `UsageGuidePage` 现在需要 `IMao_WinUI.Core.KuroSync`、`KuroProgressSyncService`、`ILocalSettingsService` 等类型，
   最后一次成功的构建日志停在 `out/main-window-harness-build.log`（2026-09-27 16:18，13 个错误）。
   它里面还有一条**路线页专属**的用例（`AutoReplanToggle` 跟随共享设置、保存被拒回滚、完成命令身份），
   在本轮改动后**依然成立**，但要等它重新编译得起来才能跑。这也是 6.3 没动手的原因。
2. 6.3 的三处「实时规划」重复：建议下一轮连同上面那条夹具一起做（把开关只留在设置页 + 游戏内工具栏，路线页降级成一行状态）。
3. 文档索引过期（§7.2）与 §7.1 的第二套退役输入链，仍未处理。

---

## 10. 修正记录（2026-10-01，同日）

### 10.1 01 卡默认了手柄玩法（用户指出）

初版 01 卡写的是「打开大地图后**按 LB** 进入『地图工具台 → 路径自动规划』」——把手柄当成了唯一入口。
实际**键鼠才是零前提的那条路**：大地图底部中央一直有一个圆形按钮，鼠标点它就开工具台（§1.1 的代码证据），
LB 只是手柄的等价入口。这个项目里键鼠用户本来就能完成全部路线操作，文案不能只教手柄。

已改：

| 位置 | 改成 |
|---|---|
| `IMao-WinUI/Views/FunctionPage.xaml` 01 卡 | 分两行并列写清：**键鼠**点大地图底部中央的圆钮；**手柄**按 LB。并补上键鼠侧的实情：框选/套索用鼠标拖动画、`Shift + 左键` 临时框选、`Esc` 取消未提交选区；手绘在大地图点一下记一个点 |
| `IMao-WinUI/Models/RoutePlanningState.cs:28` | 空状态默认文案从「打开游戏大地图后开始选点。」改成「…键鼠点底部中央的圆钮，手柄按 LB。」 |

审计文档本身也按同一处证据更正了 §1（原来写的"只有手柄 LB"是错的），并新增 §1.1 记录两条入口的代码位置。

### 10.2 提醒后来者

- 这个工具的三个面（桌面页 / 游戏内工具窗 / 覆盖层画布）**输入方式一律是键鼠与手柄并列**：
  点选、框选、套索、指定起点、手绘加点、Esc 取消都有键鼠路径。
- 只有"在哪按键"的部分才需要分输入方式写；**功能说明不该带输入方式前提**。
- 游戏内的工具台入口**不在游戏菜单里**，是那个圆钮 + LB；写帮助文案时两条都要给。
