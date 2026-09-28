# 大地图标记闪烁：玩家 2026-09-28 傍晚实测的日志分析

分析对象：`Logs_comp.zip`（结束时 `events-20260928.jsonl` 已写到 18:53:40）。
两份代码对照：仓库当前树（≈`2026.9.28.2`）与 `v2026.9.9.6` / `v2026.9.26.1` 标签。

---

## 〇、下午（15:30 以后）也试过两份 —— 但那次旧版测试**是无效的**

日志里两份构建可以用**记录形态**干净分流（`overlay-motion` 字段数、`game-state` 是否带
`compassVerified`/`compassAgreement`），下午的排布是：

| 时刻 | 构建 | 资源状态 | 证据 |
|---|---|---|---|
| 15:26 ~ 16:00 | 旧版（窄记录）| **正常** | `overlay-motion` 8 字段、`game-state` 12 字段；大地图反复开 |
| 15:53:40 | 旧版被重启 | ❌ **预加载失败** | `resource-load-failed stage=preload error=[json.exception.type_error.316] invalid UTF-8 byte at index 70: 0xF7` |
| 16:07 ~ 16:17 | 旧版反复重开 4 次 | ❌ 全部失败 | `resource-wait durationMs=0 ready=0` + `resource-load-error …0xF7` |
| 16:29:42 | 换成 **2026.9.28.1**（ASCII 目录）| ✔ 恢复 | `stage=visual-index durationMs=4779 ready=1 tiles=6565`；**UTF-8 报错消失** |
| 16:30 ~ 18:29 | 新版（宽记录）| ✔ | `overlay-motion` 46~47 字段、`game-state` 14 字段（多 `compassVerified`）|

**关键**：15:53~16:30 这段旧版**整个资源预加载失败**（`ready=0`），
按 `Docs/MapMarkerFlicker_20260928.md` 附录 B，这是
`E:\Games\明潮地图工具\…`（非 ASCII 安装路径）触发的 `type_error.316`。
`ready=0` 意味着视觉索引没建起来 ⟹ **那段时间它根本不做定位、也画不出标记**。
所以"下午旧版不闪"很可能是"下午旧版没在工作"，不能当成对照。

### 大地图标记层的撤销频率（同一台机器，同一天）

| 时段 | 构建 | `map=` 翻转/分钟 | 单次 `map=1` 持续 | 撤销时探针仍报"有图" |
|---|---|---|---|---|
| 15:26~16:00 | 旧版（窄）| **1.1** | 最长 **15 s**（18 段全是 0~15 s）| 18/18 |
| 16:30~18:29 | 2026.9.28.1（宽）| 0.8 | 最长 **136 s**，多段 43~115 s | 48 |
| 18:30~18:52 | 旧版（窄）| **2.0** | 1~16 s | 21 |
| 18:52~19:00 | 宽（另一份）| (窗口太短) | 2 / 13 / 8 s | 3 |

⟹ **在下午这段有效数据里，旧版的标记层撤销频率反而是新版的约 1.4 倍**，
而且"撤销瞬间探针仍为 1"这个签名**两份都有**。
也就是说：**"漏判一帧就撤整层"这道门，新旧版都在踩**；
它不能解释"旧版不闪、新版闪"，能解释的只有第三节那个 `focused` 门。

---

## 一、傍晚这次的实测时间线（日志里能直接读出来）

日志里同时存在**两套不同形态的构建/安装**（`overlay-motion` 的字段数不同可区分），
而 18:28~18:31 这一段正好是"先旧后新"：

| 时刻 | 事件 | 佐证 |
|---|---|---|
| 18:28:42 | 旧版 CoreHost 起 | `resource-load ... IMAO-v2026.9.26.1-win-x64`（`Map_features.imf` 缺失报警、`stage=map-features keypoints=0`、`stage=all durationMs=4071`）|
| 18:28:59 | 旧版进游戏、大地图已开着 | `game-state state=BigMap compassGoldPixels=769` |
| 18:29:01~18:29:26 | **旧版实测（约 30 秒）** | 大地图 1 s + 13 s 两段，中间玩家自己在开关图 |
| 18:30:34 | 旧版退出 | `corehost-stopped` |
| 18:30:51 | **新版起来** | `stage=map-features durationMs=406 keypoints=247389`、`stage=all ... ready=1 error=`（无 UTF-8 报错）|
| 18:31:06 | 新版第一次启动**崩了** | `native-unhandled-exception ... IMao-Core-2026-9-28-18-31-6.dmp` |
| 18:31:13 | 新版重新起来 | `app-init` + `app-ready durationMs=5` |
| 18:31:24 起 | **新版实测** | `Gameplay->BigMap`，大地图 10 s → **黑 48 s** → 亮 |
| 18:52:35 | 之后又换回另一份构建 | `IMao-v2026.9.28.1-windows-x64` |

> ⚠️ 两点需要跟玩家核对：
> 1. 玩家说的旧版是 **2026.9.9.6**，但这份日志里 18:28 跑的是 **2026.9.26.1**
>    （安装目录 `E:\Games\Wuwa_map_tool\IMAO-v2026.9.26.1-win-x64`）。
>    整个 09-23~09-28 的日志里**从未出现过 2026.9.9.6**。
> 2. 新版起来先崩了一次（18:31:06 有 dump），玩家是重启后才测的。


---

## 二、闪烁在日志里长什么样

`overlay-visibility` 只在标记层可见性翻转时记录一条（`App/App.cpp:666-677`），
所以**它的条数就是闪烁次数**；`map=` 是大地图标记层，`minimap=` 是小地图标记层。

### 旧版实测窗口（18:28:57~18:29:27）

```
18:29:12  overlay-visibility  MAP=1 rawCompass=1 rawControls=1 stable=BigMap
18:29:24  game-state          state=BigMap obs=BigMap compass=535 ctrl=1 struct=1 focused=1
18:29:25  overlay-visibility  MAP=0 rawCompass=0 rawControls=0 stable=BigMap   ← 整层被撤
18:29:26  overlay-visibility  MAP=0 mini=1  rawMinimap=1      stable=Gameplay  ← 玩家关图，状态确认
18:29:26  map-ui-transition   BigMap->Gameplay
18:29:26  map-marker-cache    cleared because big map is no longer confirmed
```
大地图这一段**只被撤了一次，而且紧接着就是状态真变 Gameplay**（玩家自己关的图）。

### 新版实测窗口（18:31:24~18:31:54，玩家开着图没动）

```
18:31:24  overlay-visibility  MAP=1 rawCompass=1 rawControls=1 rawMinimap=0 stable=BigMap
18:31:30  game-state          state=BigMap obs=BigMap compass=526 ctrl=1 struct=1 focused=1
18:31:34  overlay-visibility  MAP=0 rawCompass=1 rawControls=1 rawMinimap=1 stable=BigMap
18:31:34  game-state          state=BigMap obs=BigMap compass=526 ctrl=1 struct=1 focused=0   ← 关键
18:31:36 … 18:32:20           state=BigMap … focused=0   （连续 24 秒，探针值冻在 526）
18:32:22  overlay-visibility  MAP=1 rawCompass=1 rawControls=1 stable=BigMap   ← 48 秒后才回来
18:32:22  game-state          … focused=1
18:32:29  overlay-visibility  MAP=0 rawCompass=1 rawControls=1 rawMinimap=1 stable=BigMap
18:32:31 … 18:32:45           focused=0   （又是 16 秒）
18:32:45  overlay-visibility  MAP=1
```

**这一段就是玩家看到的东西**：地图一直开着、探针一直说"有图"，标记层却在出圈/回来。
玩家 30 秒内看到的是"亮 10 秒 → 黑 → 亮"。

---

## 三、根因：翻转不是探针判错，是 `focused` 这个门

`OverlayVisibilityPolicy::Observe`（修复前，`IMao-Core/src/App/OverlayVisibilityPolicy.h`）：

```cpp
const bool live = focused && frameId != 0;
const bool mapUp = MapUiStateController::IsStableBigMap(stableState);
const bool showMap = live && ((mapEvidence && mapUp) || (…Held(capturedAt, lastMapEvidenceAt_)));
```

> ⚠️ 版本号说明：玩家报的是"旧版 2026.9.9.6"，但日志里**两份构建都不是 9.9.6**
> （9.9.6 的 `overlay-motion` 只有 8 个字段且**没有** `overlay-window-visibility`，
> 而下午那份窄记录构建有后者）。所以本文按日志分流叫"旧版/新版"，
> **不对应具体版本号**；要发版说明时以"这台机器上跑的两份构建"为准。

`focused` = `DrawItemBase::IsMarkerDisplayContext(hwnd)`（`App/App.cpp:715`），
一旦**游戏窗口不是前台**就为 `false` ⟹ `live=false` ⟹ `showMap=false`
⟹ 标记层整体撤销 ⟹ 渲染端 `ImGuiOverWindows.cpp:815` 的 `mapVisible && AllowsMap(...)` 不成立
⟹ 那一帧不画任何标记和路线（`ImGuiOverWindows.cpp:815-830`）。

也就是说：**标记闪烁与"大地图探针有没有判错"完全无关**，
18:31:34 和 18:32:29 那两条撤销记录里 `rawCompass=1 rawControls=1` 清清楚楚。
`anchorFresh`（1200 ms）和 `kEvidenceHold`（250 ms）这两道新增的宽限**都救不了它**——
它们只宽限"这一帧没证据"，而 `live=false` 是更外层的否决。

### 为什么只有新版被测出闪烁

日志能确证的是"**新版实测窗口里门被踩到了，旧版实测窗口里没有**"：

1. **旧版实测窗口里，玩家开着图的那 13~19 秒 `focused` 一直是 1**
   （18:29:14/16/18/20/22/24 全是 `focused=1`），门没被触发。
2. **新版实测窗口里，18:31:34 起 `focused` 掉到 0 并持续 24 秒**，
   同一时刻探针还在报 `rawCompass=1 rawControls=1` ⟹ 门被触发，层被撤。
3. 更糟的是：新版在 `focused=0` 期间**大地图探针读数冻住了**
   （`game-state` 里 `compassGoldPixels` 逐条都是 526、`minimapMatches` 逐条都是 5），
   说明这几秒没有新帧进来，于是恢复只能等下一次抓到新帧——实测等了 **48 秒**
   （18:32:22 才回来）。旧版是 `focused` 一恢复（18:29:01、18:29:12）当帧就画上了。

> 这是"日志能证明的部分"。两份构建的这段门代码逐字相同，所以**不能**说
> "新版新增了这个 bug"；能说的是：这次实测里旧版没踩到、新版踩到了，且新版恢复慢 48 秒。

### 量化对比

按 `overlay-visibility` 的 `map=` 翻转统计（同一台机器、同一天、同一个玩家）：

| 指标 | 另一份构建（`overlay-motion` 47 字段）| 新版（`overlay-motion` 8 字段）|
|---|---|---|
| `overlay-visibility` 记录数 | 35 | 127 |
| 大地图标记层翻转次数 | 16 | **38** |
| 翻转率（次 / 分钟）| 0.5 | **1.9** |
| 单次 `MAP=1` 持续 | 1 / 13 / 19 / 5 / 4 s（各段都由玩家关图结束）| 10 / 7 / 3 / 2 / 16 / 1 / 5 … **最长 329 s** |
| 撤销瞬间探针仍为 `rawCompass`/`rawControls`=1 | 8 次 | **19 次** |
| 恢复等待 | 当帧 | 最长 **48 s** |

注意两份构建的采样窗口长度不同（1972 s vs 1188 s），所以**只比"次 / 分钟"和"撤销时探针状态"**。

---

## 四、修的地方（已实施，2026.9.28.3）

1. `IMao-Core/src/App/OverlayVisibilityPolicy.h` —— 新增 `kFocusHold`（400 ms）与
   `kFocusHandoffHold`（350 ms），`Observe` 的 `live` 改由 `Observed(capturedAt)` 决定：
   焦点丢掉后的第一帧不再撤层。两个上限都**压在同一帧的 `maximumAge`（500 ms）之下**，
   所以"焦点确实离开"时浮层仍会自己过期，不会把地图留在别的窗口上面。
   `Observed()` 是公开的，判定只有一处。
2. `IMao-Core/src/App/App.cpp` / `App.h` —— `lastForegroundGameMilliseconds`（原子毫秒）+
   `NoteForegroundGame()` / `FrameObserved()`；`PublishOverlayFrame` 的 `frame.mapVisible` 与
   `frame.focused` 都改用它。于是渲染端拿到的 `markerFrame->focused` 已经是**带容忍**的值，
   不会再出现"App 按宽限画、渲染端按当帧否决"的分歧。
3. `IMao-Core/src/ImguiDraw/ImGuiOverWindows.cpp` —— 渲染闸门去掉 `frame->focused`
   （那是同一道门的第二处硬否决，且与 2 重复）。
4. 焦点交接 `mapReady` 里的 `markerFrame->focused` 循环依赖因此自然解开：它现在读到的是带容忍的值，
   在交接窗口内仍然为真，那道保护不再于最需要它的那一帧失效。
5. `IMao-Core/tests/OverlayVisibilityTests.h` —— 原来那条
   "unfocused game UI cannot show markers"（单帧丢焦点即撤层）改写成焦点宽限的四条断言：
   单帧不撤、连续多帧不撤、超过上限要撤、恢复焦点当帧恢复。

> ⚠️ 仍未解决、也无法从这份日志定案的：这台机器为什么会把 `focused` 判成 0（48% 的时间）。
> 宽限只消掉"一帧就撤"，焦点长时间为 0 时浮层照样会在 350~500 ms 后自己过期。
> 上面第 4 节末尾那段 `foreground-mismatch` 诊断还没加——下次要定案就得靠它。

### 还需要玩家确认的

1. 下午 15:53–16:30 旧版 `ready=0` 那段时间，他看到的工具是什么样（有没有标记）？这能直接判定
   "旧版不闪"是不是"旧版没工作"。
2. 旧版是不是装在 `E:\Games\明潮地图工具\`（中文路径）下？附录 B 的修复值得先落地。
3. 新版实测那次黑屏的 48 秒里，他有没有点过别的窗口（触发 `focused=0`）。


---

## 五、附带发现（与本次闪烁无关，但值得单独修）

* 新版第一次启动在 **18:31:06 崩了一次**，留下
  `C:\Users\Gaiest\AppData\Local\IMao-WinUI\CrashReports\IMao-Core-2026-9-28-18-31-6.dmp`。
  这份日志里没有崩溃原因，需要单独看 dump。
* 新版这次跑的是**旧目录** `E:\Games\Wuwa_map_tool\IMAO-v2026.9.26.1-win-x64` 里报缺
  `Map_features.imf` 的那套资源布局，而新版（18:30:51 那份）`stage=map-features keypoints=247389`、
  `stage=all ready=1 error=` 是干净的。
* 同一份日志里 18:20~18:28 有 **8 分钟**的 `game-state` 冻结
  （`compass=769`、`focused=0` 一直不变），期间 `overlay-window-visibility visible=0 reason=idle`，
  浮层一直没显示——那是玩家把地图开着挂机去装新版了，不是 bug。

---

## 附录：本次复现用到的查询

日志解包在 `.logs-analysis/logs-20260928/`。

* 构建分流：`overlay-motion` 的字段数（新版 8 个 / 另一构建 47 个）。
* 闪烁计数：筛 `"message":"overlay-visibility"`，按 `map=` 变化计数。
* 根因定位：把 `game-state` 的 `focused=` 与同一秒的 `overlay-visibility` 对齐
  （见 `.logs-analysis/sidebyside.py`、`focus.py`、`final.py`）。
