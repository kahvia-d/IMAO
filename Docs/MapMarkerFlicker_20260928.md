# 大地图/小地图标记"一闪一闪"（玩家日志分析，2026-09-28）

起因：一名玩家（日志目录 `%LOCALAPPDATA%\IMao-WinUI\Logs`，`events-2026092*.jsonl`）反馈
**大地图上的标记会一闪一闪的**，并附上了 09-23 / 09-24 / 09-27 / 09-28 四天的日志。
反馈同时明确：**九成九九的用户不会这样**，所以这不是一个"人人都有"的表现。

分支：`fix/player-log-20260928`（从 `main` @ `c9a4ef5` 检出）。第六节的方案已按用户指示实施，
发行为 `2026.9.28.2`（程序分片增量更新），实施范围见文末「附录 C」。

> 注：同一批日志里还有一个**独立**的问题——`E:\Games\明潮地图工具\…`（非 ASCII 安装路径）
> 导致 `stage=preload error=[json.exception.type_error.316] invalid UTF-8 byte at index 70: 0xF7`，
> 整个资源预加载失败。那是另一条因果链，不在本文范围，见文末附录 B。

---

## 一、症状在日志里的量化

`overlay-visibility` 只在**标记层可见性发生翻转**时记录（`App/App.cpp:661`），
所以它出现的次数本身就是闪烁次数。

| 时段 | `overlay-visibility` 条数 | `map`/`minimap` 翻转次数 |
|---|---|---|
| 00:00–00:59 | 45 | 39 |
| 01:00–01:59 | 142 | 89 |
| 13:00–13:59 | 196 | 165 |
| 14:00–14:59 | 90 | 75 |
| **15:00–15:59** | **303** | **265** |
| 16:00–16:59 | 45 | 11 |

翻转最密的分钟：13:27 = 37 次、13:26 = 37 次、15:24 = 32 次、15:16 = 25 次、13:25 = 24 次、
15:13 = 23 次（**一分钟里标记层通断 37 次**）。16:00 之后（换目录、重装资源）降到 11 次。

再看"每次可见持续多久"：

* `minimap=1` 的片段共 **325 段**：0 s（同一秒内就翻回去）= 86、1 s = 45、2 s = 23、3 s = 19、4 s = 23
  → **≤4 秒的占 60%（196/325）**。
* `map=1`（大地图标记）的片段共 **84 段**：0 s = 10、1 s = 11、2 s = 9、3 s = 6、4 s = 6
  → **≤4 秒的占 50%（42/84）**。

最极端的是**帧级交替**（frame 号即捕获帧号，13:25:40，小地图模式）：

```
13:25:39  frame=45929  map=0 minimap=0 rawMinimap=0 rawCompass=0 stableState=Gameplay
13:25:40  frame=45947  map=0 minimap=1 rawMinimap=1 rawCompass=1 stableState=Gameplay
13:25:40  frame=45949  map=0 minimap=0 rawMinimap=0 rawCompass=0 stableState=Gameplay
13:25:40  frame=45952  map=0 minimap=1 rawMinimap=1 rawCompass=0 stableState=Gameplay
13:25:40  frame=45955  map=0 minimap=0 rawMinimap=0 rawCompass=0 stableState=Gameplay
13:25:41  frame=45975  map=0 minimap=1 ...
13:25:42  frame=46000  map=0 minimap=0 ...
13:25:42  frame=46003  map=0 minimap=1 ...
13:25:42  frame=46010  map=0 minimap=0 ...
13:25:42  frame=46020  map=0 minimap=1 ...
```

**标记层每 2~10 个捕获帧就被撤销一次**（按他 20~60 fps 的捕获率 ≈ 5~15 Hz 通断）。
注意 `rawMinimap` 与 `rawCompass` **跟着一起翻**——是**原始探针输出本身在相邻帧之间来回翻**，
不是状态机抖动（`stableState` 全程是 `Gameplay`，没变）。

大地图侧同一天也有（13:08:44 开图后）：

```
13:08:44  map-ui-transition  Gameplay->BigMap
13:08:46  map-ui-transition  BigMap->Unknown
13:08:46  map-marker-cache   cleared because big map is no longer confirmed
13:08:46  map-ui-transition  viewport markers cleared; player hint preserved
13:08:46  map-viewport-reset reason=map-ui-transition
13:08:47  map-ui-transition  Unknown->BigMap
13:08:47  map-ui-transition  big-map viewport session requested
13:08:47  map-viewport-reset reason=map-ui-transition
...
13:08:56  BigMap->Unknown → 13:08:58 Unknown->BigMap
13:09:01  BigMap->Unknown → 13:09:02 Unknown->BigMap
13:09:15  BigMap->Unknown → 13:09:15 Unknown->BigMap（同一秒）
13:09:19  BigMap->Unknown
```

而这期间**地图一直开着、视口定位一直在成功**：

```
13:08:45  map-viewport-result accepted=true scene=1 ... matches=56 inliers=53 inlierRatio=0.946
13:08:48  map-viewport-result accepted=true ... matches=58 inliers=57
13:08:49  map-viewport-result accepted=true ... matches=74 inliers=73
13:09:15  map-viewport-result accepted=true ... matches=77 inliers=71
```

即：**不是玩家关了地图，是我们自己的判定被打断**。

---

## 二、机制：漏一帧 = 标记全灭（回溯性撤销）

判定链是「每帧探针 → 每帧可见性 → **作废所有已发布的标记帧**」。

1. **每帧探针**（`App/App.cpp:750-772`）：`IsExistMinMap`（小地图 SURF）、
   `MapUiVisualDetector::DetectBigMapCompass`（罗盘金色像素 + 模板）、
   `DetectBigMapControlLayout`（缩放条字形），再经 `MinimapHudEvidence::Observe` 收敛。
2. **每帧可见性**：`App/App.cpp:778` 和 `:849` **同一帧调两次** `publishVisibility(...)`，
   输入是这一帧自己的探针结果；判定规则写在 `App/MapUiStateController.h:41-54`：

   ```cpp
   bool Probed() const { return controlsVisible || compassVerified ||
       (compassVisible && structureConfirmed && !structureRequiresControls); }
   inline bool BigMapMarkersVisible(const MapFrameEvidence& e) { return !e.minimapVisible && e.Probed(); }
   ```

3. **回溯性撤销**（`App/OverlayVisibilityPolicy.h:38-52`）：

   ```cpp
   if (showMap != frame_.mapVisible || showMinimap != frame_.minimapVisible)
       frame_.visibleSinceFrame = frameId;      // ← 只要翻一次，就把"从这个帧起才算数"往前推
   ```
   而 `AllowsMap/AllowsMinimap`（同文件 `:23-29`）要求 `markerFrame >= visibleSinceFrame`，
   渲染侧在 `ImguiDraw/ImGuiOverWindows.cpp:815` 执行这个门：
   `if (frame->mapVisible && visibility->AllowsMap(frame->frameId))`。
   **于是任何一帧漏测都会让"上一批已经算好的标记帧"当场作废**，必须等新的一帧标记帧发布才回来。
   `App.cpp:775-778` 的注释说明这是故意的（"Revoke already-published marker frames before any slower
   map verification … must not keep an absent map on the screen"）。

4. **关键：状态机有迟滞，标记可见性没有**。
   `App/MapUiStateController.cpp:27` 要**连续 10 次** Unknown 才退出 BigMap，`:43` 要连续 2 次才进入；
   而 `App/MapUiStateController.h:58-60` 的注释直说了：

   > OverlayVisibilityPolicy independently hides markers on the first missing observation
   > instead of waiting for this debounce.

   在他这台机器上"连续 10 次"只等于 0.3~2 秒（探针节拍 ≈ 20~36 Hz），所以状态也一起掉——
   每掉一次就触发下面整条放大链。

---

## 三、放大链：一次瞬时 Unknown 会做四件事

`App/App.cpp:878-911`（`update.changed`）：

| 动作 | 位置 | 后果 |
|---|---|---|
| `DrawItemOnGameMap::ClearNearItemsData()` + `DrawRouteOnMap::ClearRountsData()` | `:882-886` | 大地图标记/路线数据被清空（日志 `map-marker-cache cleared…`） |
| `mapViewportResetRequested = true` | `:896-899` | **整个视口会话被重置**（日志 `map-viewport-reset` / `viewport markers cleared`），要重新做一次 512 范围绝对定位才能再画（他单次定位 330~400 ms） |
| `coordinateSuspendRequested = true` | `:891-894` | 玩家位置被挂起（`player location suspended`） |
| `mapViewportStartRequested = true` | `:901-905` | 回来后重新开会话（又是 `map-viewport-reset`） |

**第五件事在浮层窗口本身**：窗口矩形挂在"最新标记帧"的 `mapVisible` 上。

```cpp
// ImGuiOverWindows.cpp:599-600
bigMapFrame = markerFrame && markerFrame->mapVisible;
minimapWindowRequested = !forceFullOverlay && markerFrame && !markerFrame->mapVisible;
// :613-618  → 窗口被缩成 MiniOverlayClientRect
```

而 `mapVisible` 的定义是（`App/App.cpp:2620`）：

```cpp
frame.mapVisible = isOpenMap && enabledMapShowItem && viewport.confidence >= 2 && !captured.image.empty();
```

于是**大图视口一旦不稳，整块 1920×1080 的大地图浮层会缩成 1241×260 的小地图面板**。日志实锤：

```
16:51:24  game-state   state=BigMap observed=BigMap ...（地图开着）
16:51:25  overlay-motion overlayMode=full  w=1920x1080 markerDraws=22
16:51:59  overlay-motion overlayMode=mini  w=1241x260      ← 地图还开着，浮层缩成小面板
16:52:15  overlay-motion overlayMode=mini  w=1241x260
16:52:17  overlay-motion overlayMode=full  w=1920x1080      ← 16 秒后又回来
```

`ImguiDraw/ImGuiOverWindows.cpp:583-588` 的注释记录着同一个坑**以前踩过一次**：

> Keying this on "the minimap was positively detected" instead made the window jump to full size at
> every state transition, startup included, which is both the visible flicker and exactly the
> full-screen composition cost this change exists to avoid.

——现在换成了按 `markerFrame->mapVisible` 判定，等于用另一个每帧信号重新引入了同类闪烁。
（16:07–16:55 一段里 `mini↔full` 共翻转 26 次。）

---

## 四、为什么只有这名玩家

因为**他机器上交到我们手里的帧本身在相邻帧之间交替变化**，原始探针跟着翻：

```
13:08:46  game-state  state=BigMap observed=Unknown minimapMatches=0   compassGoldPixels=0   mapControlsVisible=0
13:09:02  game-state  state=BigMap observed=BigMap   minimapMatches=0   compassGoldPixels=527 mapControlsVisible=1
13:09:14  game-state  state=BigMap observed=Unknown minimapMatches=0   compassGoldPixels=0
```

* 罗盘探针是固定裁剪上的**纯颜色测试**（`MapUiVisualDetector.cpp:198-242`，
  参考值约 8% 金色像素，他这边稳定帧是 527~567）——相邻帧 527 ↔ 0。
* 小地图探针是 SURF 匹配 `IconTask` 特征，`minimapMatches` 在 10~14 ↔ **0** 之间跳。

这些是纯模板/颜色判定，没有跨帧记忆；**帧稳定的人永远不会翻**，所以其余用户碰不到第二节那套
"漏一帧 = 全灭"。他这边则是 yes/no/yes/no，于是标记层就跟着 5~15 Hz 通断。

同时他的**大图视口置信度长期在 1**（`confidence >= 2` 不成立）：

| overlayMode | 采样数 | 平均 sourceFps（新标记帧/秒） | 平均 captureFps |
|---|---|---|---|
| `full`（大地图） | 203 | **4.36** | 21.61 |
| `mini` | 262 | 10.54 | 21.00 |
| 无字段段（更早的构建） | 3643 | 12.15 | 29.69 |

即开图时**只有约 20% 的捕获帧能产出新的可靠标记帧**。原因见
`App/MapViewportPredictor.cpp:220-224`：连续 3 次帧间跟踪失败就开始扣 `confidence`，
而回血只能靠一次绝对定位 `Confirm`（`:165` → 3），他一次定位要 330~400 ms。
帧率越低（`capture-cadence` 只有 20~26，开图 36~38；`sourceFps` 最低 3.5），
相邻地图帧差得越远，帧间跟踪越容易连续失锁。

---

## 五、待确认的机器侧诱因

"帧为什么在交替"需要他那边再给一份数据。候选（按可能性排序）：

1. **帧生成 / 超分**（DLSS 3 / FSR FG / AFMF 之类）：每隔一帧是插值帧，HUD 的亚像素渲染不同，
   SURF 与模板隔帧失败——非常符合"隔 2~3 帧翻一次"的形态；
2. 任务图标 / 罗盘裁剪区域内有 HUD 动画元素；
3. 帧率过低导致帧间地图跟踪频繁失锁（会让 `confidence` 长期 1，放大第三节的后果，
   但**解释不了罗盘金色像素归零**）。

确认手段：工具已有 `Diagnostics::SaveImage("state-change-full", stateSnapshot)`
（`App/App.cpp:881`，随诊断开关走）。让玩家打开诊断复现一次，就能直接看到"探针说没有"的那一帧；
同时问一下他的分辨率 / UI 缩放 / 是否开了帧生成 / 显卡型号。

---

## 六、修复方案（已实施，见附录 C）

1. **给标记可见性加证据宽限**：一层界面在**自己的**探针短暂漏判时保持可见，
   宽限 250 ms（`kEvidenceHold`），恢复时立即显示；单帧漏测**不**更新 `visibleSinceFrame`。
   另一层界面的证据、失焦、稳定状态改变都在当帧撤销，所以关掉地图仍然会在小地图出现的那一帧
   隐藏大地图标记。位置：`App/OverlayVisibilityPolicy.h`。
   这一条单独就消掉日志里绝大多数闪烁（265 次/小时那一类）。
2. **把"地图是否开着"和"这一帧能不能摆标记"分开**：标记层的可见性跟**已经带迟滞的界面状态**走
   （和 `minimapVisible` 原本的做法一致），能不能摆由 `ImageAnchoredOverlay::Update` 自己拒绝
   不可靠的位姿（`Runtime/ImageAnchoredOverlay.h:64` 已经在做）。
   这样一次定位抖动不再发布"没有地图"的标记帧，也就不会通过
   `ImguiDraw/ImGuiOverWindows.cpp:599-600` 把整块整屏浮层缩成小地图面板（16:51:59–16:52:15 那 16 秒）。
   位置：`App/App.cpp` 的 `PublishOverlayFrame`。
3. **画布匹配本身也能证明"地图还开着"**：新增 `MapFrameEvidence::anchorFresh`
   （最近 1.2 s 内有被接受的绝对视口解），并入 `Probed()`。
   探针读的是控件，会连续一两秒失明；画布特征匹配不会（他那边这段时间一直是 74~100 内点）。
   位置：`App/MapUiStateController.h`、`App/App.cpp`（`MapViewportAnchorFresh`、
   `CommitMapViewportResult`、`BeginMapViewportSession`）。
4. **瞬时 Unknown 不再重置视口会话**：只有确认回到 Gameplay 才 `map-viewport-reset`，
   下一次进入大地图本来就会 `BeginMapViewportSession` 开新会话，所以旧 anchor 不会被复用到新位置。
   位置：`App/App.cpp` 的 `map-ui-transition` 处理块。
5. 第 4 条建议里的"翻转时补诊断"没有做：本次改动的三条信号（宽限、稳定状态、anchorFresh）
   已经能从既有 `overlay-visibility` / `map-viewport-*` 记录里读出来，等玩家实测反馈再决定要不要加。

顺手记一笔（不是本 bug）：`map-marker-cache: cleared because big map is no longer confirmed`
（`App.cpp:885`）名不副实——正常游玩里每次 `Unknown->Gameplay` 都会打，一天几百条，
很容易被当成大地图问题的证据。真正的大地图证据是紧跟 `BigMap->Unknown` 的
`viewport markers cleared` + `map-viewport-reset` 组合。

---

## 附录 A：复现所用查询

日志解包在 `.logs-analysis/extract/`（已加入 `.git/info/exclude`，不入库）。

* 可见性翻转次数/时长：筛 `"message":"overlay-visibility"`，按 `map=`/`minimap=` 变化统计
  （时间戳只到秒，故有 "0 s" 段；用 `frame=` 号可看出真实帧间隔，见第一节）。
* 帧级交替：按 `frame=` 号排序列出 13:25:38–13:25:43、13:26:03–13:26:07。
* 视口可靠性：`overlay-motion` 的 `overlayMode=` / `sourceFps=` / `captureFps=` 每 2 秒一条
  （仅 2026.9.28.1 之后的构建有这些字段）。
* 捕获方式与帧数：`capture-wgc-frames`（`roiFrames`/`fullFrames`/`skipped`）、`capture-cadence`。

## 附录 B：同批日志里的另一个独立问题（非本文范围）

玩家在 `E:\Games\明潮地图工具\IMao-v2026.9.28.1-windows-x64\`（非 ASCII 路径）启动时：

```
stage=preload error=[json.exception.type_error.316] invalid UTF-8 byte at index 70: 0xF7
stage=all durationMs=1 ready=0 error=[json.exception.type_error.316] invalid UTF-8 byte at index 70: 0xF7
```

成因：`Assets/FeaturesDatas/Map_features.imf` 在 2026.9.28.1 布局里已不再随包发布，
错误信息用 `path.string()`（Windows 上是 ANSI/GBK）拼出该路径
（`Feature/Processing/FeatureBinaryCodec.cpp:198`），
再由 `Runtime/StructuredLogger.cpp:117` 的 `record.dump()` 序列化时被 nlohmann 拒绝
（`packages/nlohmann/nlohmann/detail/output/serializer.hpp:507-513` 抛 `type_error.316`），
异常被 `Feature/RuntimeFeatureRepository.cpp:426-431` 的 catch-all 接住，
一条**可选**资源告缺就此变成整个资源集不可用。字节对账：前缀正好 69 字节，
`明` 的 GBK = `C3 F7`，下标 69/70 与日志完全吻合。
玩家 16:29 换到 `E:\Games\Wuwa_map_tool\…`（纯 ASCII）后资源立刻 `ready=1`——
等于他自己验证了修复方向。

## 附录 C：本次实施（2026.9.28.2）

| 文件 | 改动 |
|---|---|
| `IMao-Core/src/App/OverlayVisibilityPolicy.h` | `kEvidenceHold`（250 ms）证据宽限；`Held()` 以捕获时间计时；`Reset()` 一并清证据时间戳 |
| `IMao-Core/src/App/MapUiStateController.h` | `MapFrameEvidence::anchorFresh` 并入 `Probed()`；更新状态机旁的注释 |
| `IMao-Core/src/App/App.h` | `lastMapViewportFixMilliseconds`（steady-clock 毫秒）+ `MapViewportAnchorFresh()` 声明 |
| `IMao-Core/src/App/App.cpp` | `kMapViewportFixFreshnessMs`（1200 ms）；`frameEvidence()` 填 `anchorFresh`；`CommitMapViewportResult` 打时间戳；`BeginMapViewportSession` 清零；`PublishOverlayFrame` 的 `mapVisible` 改由稳定状态决定；`map-ui-transition` 只在确认回到 Gameplay 时重置视口 |
| `IMao-Core/tests/OverlayVisibilityTests.h` | 改两条断言（单帧漏判不再撤层）、补宽限的边界与"从最新证据计时"两组用例 |
| `IMao-Core/tests/MapControllerUiTests.h` | 补 `anchorFresh` 作为独立大地图证据、被小地图证据否决、以及过期不计的用例 |
| `Version.props` | `2026.9.28.1` → `2026.9.28.2` |
| `Docs/Release-2026.9.28.2.md` | 发行说明（同时作为签名清单的 `app.notes` 与发行页正文） |

验证：`IMaoOptimizationTests`（含 `TestOverlayVisibility` / `TestMapControllerUi`）全绿；
`IMao-CoreHost` 重新编译通过。发布方式为程序分片增量更新（`--program-shards true`），
不带整包 ZIP；资源集未变，不带离线集合包。

未实施（留待玩家实测反馈后再定）：
* 第 4 条"翻转时补诊断"；
* 大地图变换层的短时 hold（小地图已有 150 ms，大地图目前没有；本次日志里
  `trackingMisses` 的 bursts 集中在旧构建，新构建里最大只有 6~7 帧）。
