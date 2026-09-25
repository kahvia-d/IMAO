# 路线攻略回退与攻略内长按跳过（实施与验收记录）

**状态：** 已实现并通过自动化验收；实机手感待玩家确认。键盘攻略键（F8）的空范围回退
在 §7.1 修正后才真正生效。
**分支：** `codex/route-guide-skip`（基线 `25adfde`；实现提交 `6047a63` / `7690872` / `836ce9b`，
后续修正见 §7）

> 这份文档取代了本分支上被删除的旧实施计划。旧计划在落地过程中被改成迁就实现（删掉了
> 「显示实际配置键名」「长按进度要标识动作」等约束），结果键名提示、鼠标长按与窗口级用例
> 全部缺失。这一版按"先写清行为、再写实现、最后按行为验收"的顺序记录。

## 1. 要解决的问题

玩家开着一条自动路线往前走时，如果小地图范围内没有未完成点位，按攻略键只会得到"附近没有
点位"，等于让玩家自己去大地图上找下一个点。本功能让这种情况直接打开**正在导航的下一个
待收集点**的攻略，并在"恰好是当前导航目标"的那份攻略里提供**长按跳过**。

跳过只改变这条路线的进度，**不写点位完成记录**，可以被现有的撤销跳过恢复。

## 2. 行为规格（已与用户逐条确认）

### 2.1 攻略入口（键盘攻略键与手柄 LB+X 同一条优先级链）

| 触发时的情形 | 行为 |
|---|---|
| 范围内有未完成点位 | 完全走既有逻辑：单点直接开、多点弹选择器 |
| 范围内为空且核心有当前导航目标 | 打开该目标的攻略（`navigating`、`waitingForLocation`、`paused` 一视同仁，理由见 §4） |
| 范围内为空且核心没有当前目标 | 不打开攻略，提示一句「附近没有未完成点位，也没有正在导航的路线目标。」 |
| 定位不可用、档案或筛选刚变过 | 只提示「当前位置暂不可用，请等小地图定位恢复后重试。」，**绝不**回退 |

判定依据：原生把"真空范围"(`guide-empty`) 与"定位坏了"(`position-unavailable`) 用**不同的
结果**告诉托管层，托管层只允许前者触发回退。不能靠"没有 selection 字段"推断，否则定位一抖
就会弹出无关的路线攻略。

回退的判定位置按入口分：键盘攻略键有**关联调用方**，托管层拿到 `guide-empty` 后自己查
`markerGetRouteGuide` 并打开；手柄 LB+X 没有关联调用方，由原生在空范围时补发一次
`markerGuideShortcut{gamepad:true}`。两条入口的最终行为一致（同一段目标解析与跳过资格），
但键盘那条**不能**依赖原生的补发：原生只对 `gamepad=true` 的调用补发，键盘关联调用传的是
`false`，所以托管层必须自己回退（见 §7.1）。

### 2.2 跳过资格

只有同时满足才显示跳过入口：

- 攻略点位的 `profileId + stateId + pointId` 与原生当前导航目标**完全一致**；
- 拿到活动 `routeId` 与 `revision`；
- 判定来自原生答复，**客户端路线快照只用于显示，不作为授权**。

窗口打开后资格随路线变化刷新：导航停止/暂停、路线切换、目标推进到下一点、该点已完成或已
跳过 → 立刻隐藏跳过。

### 2.3 提交跳过

复用 `routePlanning { action:"skip", profileId, routeId, key, expectedRevision }`：

- 原生已有的"当前目标 key / routeId / revision"校验是最终守卫；不是当前目标就拒绝。
- 只写路线进度，不写点位完成记录；拒绝时不改路线也不改记录。
- 成功后关闭攻略；失败保留窗口并给出可重试提示。
- 结果可被现有 `undoSkip` 撤销。

### 2.4 长按（600 毫秒）

- **键鼠：** 配置键（默认 **G**，可改可禁用）**按住 600 ms**，带进度条；窗口内「跳过」**按钮**
  就是一个按钮——**单击即提交一次**。（2026-09-25 实机反馈：按钮原来也被"按住 600 毫秒"判定拦下，
  鼠标点一下只会闪一下进度条再取消，等于没反应。）
- **手柄：** 可跳过的**详情页**上长按 **Y** 600 ms；短按 Y 保留原有"打开路线菜单"行为。
- 输入必须在攻略窗口**前台**且当前**可跳过**；窗口外、失焦、切换攻略、切到图片页、按住不足
  600 ms、混合按键 → 一律取消，不产生写入。
- 一次长按**最多提交一次**（键盘自动重复消息不得重复提交）。
- **键鼠两个通道各自计时**：先按下的通道不会被后按下的清零，也不会被后按下的提前提交。

### 2.5 可发现性

「跳过」按钮文案就是**「跳过」**（用户 2026-09-25 明确要求：按钮保持简短，键位说明不挤在按钮上）。
键鼠通道实际绑定的键名（默认 G，可改可禁用）与手柄 Y 由设置页与使用指南说明；设置页与使用指南
同步说明回退、"只有当前导航目标能跳过"、"跳过只改路线且可撤销"。

## 3. 接口约定

1. `markerGetNearbyGuide` 的关联答复在失败/空范围时带 `outcome`：
   - `position-unavailable`：定位、窗口焦点、进程或筛选版本不可用；
   - `guide-empty`：定位有效但筛选后确实没有候选；
   - `complete-empty`：完成意图下的同类空结果。
   成功路径（`selection` 单点 / `candidates` 多候选）形状不变。
2. `publish=false` 的关联调用不再发游戏内提示：回退是否成立由托管层决定，成功回退不该先报
   "附近没有点位"。手柄路径没有关联调用方，自己发一次 `markerGuideShortcut`
   （`gamepad/gameHwnd/sourceHwnd/profileId/screenX/screenY`），坐标取**游戏窗口中心**——
   桌面鼠标可能停在别的显示器，用它会让攻略开在错误的屏幕上。**这个补发只覆盖手柄路径**：
   键盘关联调用传 `gamepad=false`，托管层必须自己回退。
3. `markerGetRouteGuide` 的答复新增 `navigationStatus`，保留 `{ profileId, routeId, revision, selection }`。
4. 跳过授权只来自该答复；`key` 形如 `stateId:pointId`，与 `routeId`、`revision` 一起作为提交参数。
5. 配置字段 `GuideSkipKey` / `guideSkipKey`，默认虚拟键码 `71`(G)。核心**不做**按键处理：只保存
   绑定、参与冲突校验并同步给攻略窗口（攻略窗口按前台窗口事件处理 G）。

## 4. 关键设计决定与踩过的坑

| 曾经的做法 | 问题 | 现在的做法 |
|---|---|---|
| 键鼠共用一个按住计时器，松开任一路就 `Begin` 重启 | 先按住鼠标再按 G 会把已累计时间清零，甚至永远凑不满 600 ms | 两个通道各持一个 `GuideSkipHoldGesture`，各自计时 |
| 资格失效时直接清空按住状态 | 玩家手还按着，资格恢复后进度不再继续，只能重按 | 标记为"挂起"，资格恢复时从**当前时刻**重新起算（不跨失效期累计时间） |
| 回退以 `navigationStatus == "navigating"` 为前提 | 打开大地图时核心必然处于 `waitingForLocation`（进入大地图会清掉玩家定位），而路线工具栏正好在那里 | 打开攻略只要求"有当前目标 + 有落点"；`navigationStatus` 只用于**跳过资格** |
| 「跳过」按钮文案里带键名 | 玩家要的是简短按钮，键位说明属于使用指南 | 按钮固定「跳过」，键名只在指南与设置页出现 |
| 「跳过」按钮也按 600 毫秒 | 鼠标点一下完全没反应，按钮不像按钮（实机反馈） | 按钮单击即提交；600 毫秒只约束键盘快捷键与手柄 Y |
| 跳过键的前台判定放在窗口里 | 玩家在游戏里长按 G 时窗口拿不到键盘焦点，长按永远无效（实机反馈） | 按下/松开由原生钩子按"攻略窗口可见 + 有前台窗口"筛选后转交，窗口不再自己查前台 |
| 键盘回退等原生补发 `markerGuideShortcut` | 原生只对 `gamepad=true` 的附近查询补发，键盘关联调用传 `false`，于是 F8 在空范围上永远打不开攻略（§7.1） | 键盘在托管层自己回退：`guide-empty` 后直接查 `markerGetRouteGuide` 并打开 |
| 候选答复后不 `return`，落到"位置不可用"判定 | 候选答复是没有 `outcome` 字段的 `markerCandidates` 事件，键盘按出候选列表时会额外弹一句"当前位置暂不可用" | 候选分支自己 `return`，并加断言钉住"候选列表绝不报成定位丢失" |
| 附近-chooser 打开的攻略也去查一次路线目标 | 让"附近"这条路径依赖路线状态，并破坏既有用例的 IPC 计数 | 该路径 `resolveSkip: false`，不查（附近攻略不提供跳过） |
| `GamepadInputUpdate` 不带动作类别 | 手柄 A 完成与 Y 跳过共用一条进度通道，跳过进度会画到完成条上 | 更新里带上 `HoldAction` |

## 5. 代码位置

| 位置 | 责任 |
|---|---|
| `IMao-Core/src/ImguiDraw/Items/DrawItemOnMinMap.cpp` | `outcome` 区分空范围与定位失败；手柄空范围发起路线回退事件 |
| `IMao-Core/src/Runtime/RoutePlanningService.cpp` | `GuideTarget` 附带 `navigationStatus`；既有 `skip` 守卫与持久化不变 |
| `IMao-Core/src/Runtime/RuntimeHotkeys.h` | `guideSkipKey` 字段、冲突校验、快照与打包同步 |
| `IMao-WinUI/Services/MarkerGuideCoordinator.cs` | 仅 `guide-empty` 回退；跳过资格刷新与提交；附近路径不查路线目标 |
| `IMao-WinUI/Views/MarkerGuideWindow.cs` | 跳过入口、文案、双通道长按、取消条件、手柄进度归属 |
| `IMao-WinUI/Models/GuideSkipHoldGesture.cs` | 单通道按住计时模型 |
| `IMao-WinUI/Models/GamepadInput.cs` | `CanSkip` / `SkipGuideStop` / `HoldAction` |
| `IMao-WinUI/Models/RuntimeConfiguration.cs`、`Views/SettingsPage.xaml{,.cs}` | 跳过键的配置、界面与冲突校验 |

## 6. 验收证据（本次实跑）

| 套件 | 命令 | 结果 |
|---|---|---|
| 原生路线规划 | `x64\Release\IMaoRoutePlanningTests.exe` | `Route planning tests passed` |
| 原生路线服务守卫 | `out\auto-replan-native\IMaoRoutePlanningServiceTests.exe <dir>` | `failures=0` |
| 托管单测 | `Tests\ManagedRuntime\bin\x64\Release\net8.0\ManagedRuntime.exe` | exit 0，**669 PASS / 0 FAIL** |
| 真实窗口（攻略） | `GuideWindowRuntime.exe`（默认模式跑 GuideWindowTests + GamepadWindowTests） | **23 PASS / 1 FAIL**，见下 |
| 真实窗口（路线工具栏） | `GuideWindowRuntime.exe --test-route-controller` | **6 PASS / 0 FAIL** |
| 真实窗口（附近选择器） | `GuideWindowRuntime.exe --test-nearby-chooser` | 2 PASS / 1 FAIL（本会话前台限制，见下） |
| 生产工程 | `dotnet build IMao-WinUI\IMao-WinUI.csproj -c Release -p:Platform=x64 -r win-x64` | 0 错误（含 XAML 编译） |

攻略套件里唯一那条失败是**过期的用例**，与 F8 无关：`enlarged paging failure shows current page and
a visible error` 断言的是 `MarkerGuideWindow` 上名为 `imageDialog` / `enlargedStatus` / `enlargedPicture`
的控件，而放大看图在 `60e62de`（2026-09-20）就搬到了独立窗口 `GuideImageWindow`，这三个字段
早已不存在，用例必然超时。它此前一直没跑过，因为套件更早就在 F8 那条用例上失败（§7.1）。
要恢复这条覆盖需要按现在的 `GuideImageWindow` 重写，属于翻页/看图那条线，没有顺手改。

本会话无法执行、需要真实桌面/游戏的部分：

- `--test-nearby-chooser` 第 3 例与 `--test-gamepad-return`：断言依赖"受控窗口取得前台焦点"，
  在本会话里超时。**回退前的同一提交上这两条同样失败**，与跳过和 F8 都无关；有真实桌面的会话可直接复跑。
- `IMao-WinUI.exe` 需要提权，设置页与实机手感无法由自动化验证。
- `IMaoRoutePlanningServiceTests` 带真实的自动重排线程与 2 秒确认间隔，对磁盘负载敏感：
  有一次与其它原生套件并行复跑时出现 2 处失败，随后单独复跑 4 次全部 `failures=0`。
  那次没有把失败文本留下来，所以不能断言是它本身的问题，也不能断言不是——复跑时单独跑。

## 7. 实机验收发现的问题

### 7.1 攻略键"按了没用"：真正的原因是键盘入口根本没有回退

**实机现象**：路线正在导航、小地图范围内没有未完成点位时按 F8，什么都打不开。日志里是连续
五条 `guide-shortcut source=keyboard nearby-outcome='guide-empty'`，后面没有任何
`guide-activation`；同一天的 `source=gamepad ... hasSelection=True` 却能正常开窗。

**根因**：空范围回退当时只实现在原生，而且带了 `gamepad` 条件：

```cpp
if (guide && gamepad && publish && OpenRouteGuideFallback(observation.gameHwnd))   // 键盘那条传的 gamepad=false
```

键盘攻略键走的是**关联调用**（`CoreHostMain.cpp` 里 `HandlePlayerNearbyAction(true, false, 0, false)`，
`publish=false`），原生因此从不补发回退事件；托管层拿到 `guide-empty` 就 `return`，两头都不动。

**修法**：键盘在托管层自己回退——`guide-empty` 之后不再返回，直接走与手柄完全相同的那段
`markerGetRouteGuide` 解析（同一份答复同时用于跳过资格，因此不多查一次）。定位丢失
(`position-unavailable`) 仍然只报错、绝不回退。空范围且核心没有当前目标时，提示用户确认过的
那一句「附近没有未完成点位，也没有正在导航的路线目标。」。

**顺带修掉**：候选分支原来不 `return`，会落到"位置不可用"那行判定上，于是键盘按出候选列表时
还会额外弹一句"当前位置暂不可用"——候选答复是 `markerCandidates` 事件，根本没有 `outcome` 字段。

**此前的一个错误结论**（保留在此，避免重复）：本轮早些时候曾判定"按键自动重复导致钩子对每条重复
消息都入队一次开/关"，并据此改了钩子。这个因果**不成立**，已用用例证伪（`RoutePlanningTests` 的
攻略键语义用例）：`PlanningEscapeKey::Handle` 对重复 key-down 返回 `Consume`，只有第一次按下返回
`ReturnToPan`，钩子判定本来就是 `action == ReturnToPan`。后加的 `firstDown` 条件不改变任何行为
（保留它只是把"一次按下=一次请求"写成显式条件，防止以后改状态机时退化）。"连按 N 次 = 开→关→开"
的观感问题真实存在，但它不是"按一下没反应"的原因。

**回归网**：`Tests/GuideWindowRuntime` 的 fixture 现在回答 `markerGetNearbyGuide`
（`outcome=guide-empty`），于是 F8 那条路真的会被走通，并有四条用例钉住：空范围回退到当前目标
（并显示跳过）、定位丢失绝不回退、空范围且无目标时只报那一句合并提示、以及"每次打开只查一次
权威目标"的 IPC 计数。

### 7.2 手柄长按 Y 无效：资格里混进了"窗口必须是前台窗口"

长按 Y 由输入服务按手柄上下文分发（它自己已经校验过窗口），但攻略窗口把
`IsGuideForeground()` 也算进了 `CanSkip`。实测手柄打开攻略后窗口常常不是前台，
于是 `CanSkip` 恒为 false，Y 永远落回短按语义——日志里 `skip-eligible=1` 之后紧接着
就是一串 `action OpenRouteMenu`。

修法：`CanSkip` 只表示"这个点位是当前导航目标"；键鼠通道另用
`CanSkipFromKeyboardOrPointer`（要求前台，因为键盘事件与鼠标点击本来就只送到前台窗口）；
计时器只对键鼠通道做前台检查。

### 7.3 已另行修复的相邻问题

路线规划工具栏左摇杆"只能上下、不能右推"已在同一轮修掉，根因与本功能无关（页面在"草稿为空 +
有活动路线"时「开始选点」与「新建路线」共用指令键 `new`，而选中身份用的是键，重建导航表时
永远命中第一个）。详见 [地图工具台](MapTools_20260908.md) 的 2026-09-25 小节。


## 8. 诊断记录（排查用，长期保留）

`%LOCALAPPDATA%\IMao-WinUI\Logs\gamepad-*.jsonl` 里：

| message | 用途 |
|---|---|
| `guide-route-fallback` | 空范围要回退：原生那一侧（手柄组合键）记 `reason=guide-empty`；托管键盘入口记 `reason=guide-empty source=keyboard` |
| `guide-shortcut` | F8 / 手柄两条入口各收到什么；键盘那条缺失即表示按键没进来 |
| `guide-skip` | 跳过资格逐条分支与不合格原因；提交与接受结果 |
| `guide-skip-hold` | 窗口侧按住通道与资格快照（同状态只记一次，不刷屏） |
| `route-toolbar` | 工具栏实际送出的摇杆值 |

## 9. 实机验收清单（请玩家确认）

- [ ] 大地图上开一条路线并开始导航：走到没有未完成点位的区域按攻略键 → 打开当前目标攻略。
- [ ] 同一操作在大地图界面（定位为 `waitingForLocation`）也要能打开。
- [ ] 路线中较后的点位：即使手动打开它的攻略，也不显示跳过。
- [ ] 当前目标攻略：按住 G（或「跳过」按钮 / 手柄 Y）0.6 秒 → 推进到下一个点；按住不足 0.6 秒不生效。
- [ ] 按住鼠标的同时再按 G：不要互相清零，任一通道满 0.6 秒即提交一次。
- [ ] 跳过之后用路线页的撤销跳过恢复，完成记录不变。
- [ ] 改键/禁用跳过键后，攻略窗口按钮文案跟着变；禁用后按键不再触发。
- [ ] 窗口外按 G 不触发跳过；手柄 Y 在别处仍是路线菜单。