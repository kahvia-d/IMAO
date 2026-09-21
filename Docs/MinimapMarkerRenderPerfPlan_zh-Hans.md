# IMAO 小地图标记渲染掉帧优化任务书

> **这份文件是本次性能优化工作的任务书 + 进度台账，不是完成后的一次性记录。**
>
> - 分支：`perf/minimap-marker-render`，从 `main` = `688e100`（`Say what a release note has to be before it is signed`）切出。
> - §1～§19 是**任务书原文**，逐字保留，避免走样或遗忘；§20 起是**开工前的现状核实**与**进度台账**。
> - 记录纪律：**没有实测的数据一律写「未验证」**。禁止写"预计已解决 / 应该已经优化 / 理论上不会掉帧"。
> - 每完成一个阶段就地更新 §21 台账、并把该阶段的状态行改成「已实现（待实测）」或「已验证」。
>
> 状态：**Phase 0～Phase 4 已落地并实测。Phase 4 的 A/B/A/B 给出明确收益（+9.6 fps、p95 −2.4 ms、>20ms→0，见 §28）⟹ 按任务书 §14 值得正式落地；Phase 5/6 未开始，下一步是"正式设计 Full/Mini 切换"。**（明细见 §21）

---

## 1. 任务背景

当前仓库：

`https://github.com/kahvia-d/WWMAP-TOOLS`

现象：

当 IMAO 开启小地图点位标记渲染后，游戏帧率通常下降约 10～20 FPS，并伴随明显的帧时间长尾和“不丝滑”现象。

仓库已有性能分析：

`Docs/GameFrameCostAnalysis_20260918.md`

历史测试已经表明：

* 掉帧问题与 Overlay 绘制链路高度相关。
* 单纯关闭 `ClearRenderTargetView` 几乎不能解决问题。
* WGC 捕获链已经做过一定程度的限流和缓冲复用优化。
* DirectComposition 与传统 layered window 哪个更快，目前没有足够可靠的结论，不允许将其直接视为本轮解决方案。
* 当前普通游戏状态下，为了只在左上角小地图附近显示少量标记，依然维护了接近整个游戏客户区大小的 Overlay HWND 和 SwapChain。
* 小地图标记绘制过程中仍存在一些可以明确优化的 CPU 查询、纹理查找和 draw-call 开销。

本轮目标不是重新设计整个 Overlay 系统，而是：

**先使用低风险改动削减明显浪费，再通过严格 A/B 测试验证“独立小尺寸 MiniMap Overlay”是否能够显著改善帧率。**

---

# 2. 核心原则

本任务必须遵守以下原则：

1. 禁止无关重构。
2. 禁止为了“代码更漂亮”修改与性能问题无关的模块。
3. 禁止修改视觉定位算法、坐标定位算法、地图匹配算法的核心行为。
4. 禁止修改现有用户数据格式。
5. 禁止改变点位完成逻辑、路线规划语义和标记筛选结果。
6. 禁止直接将 DirectComposition 改成默认方案。
7. 每一步必须能够独立测试和回滚。
8. 性能结论必须来自同一次运行中的交替 A/B 测试，禁止用不同时间、不同游戏场景下的绝对 FPS 横向比较。
9. 优先复用现有性能诊断和 PresentMon 测量体系。
10. 不要创建无需求的架构层、抽象基类、manager、factory、service 等。

本轮遵循：

`测量 → 小改动 → 测量 → 再决定是否继续`

而不是：

`猜测 → 大重构 → 看起来应该更快`

---

# 3. 第一阶段：移除小地图渲染线程中的完成状态实时查询

涉及重点文件：

`IMao-Core/src/ImguiDraw/Items/DrawItemOnMinMap.cpp`

`IMao-Core/src/ImguiDraw/Items/DrawItemBase.cpp`

`IMao-Core/src/Runtime/MarkerCompletionStore.h`

当前 `DrawItemsOnMinMap()` 中存在类似逻辑：

```cpp
if (DrawItemBase::IsPointCompleted(frame.sceneName, item))
    continue;
```

而 `IsPointCompleted()` 最终会进入 `MarkerCompletionStore::Completed()`，其内部需要获得 mutex，并查询完成状态。

这意味着 Render Thread 可能对每一个 marker、每一帧重复查询完成状态。

但 `UpdatePlayerNearItemsData()` 已经为 marker 填充：

```cpp
tempItemData.isSaved = isSaved;
```

## 修改目标

小地图绘制阶段应优先直接读取当前 `ItemMarkerFrame` 中已经冻结的状态：

```cpp
if (item.isSaved)
    continue;
```

不要在 `DrawItemsOnMinMap()` 每帧重新调用：

```cpp
DrawItemBase::IsPointCompleted(...)
```

## 必须确认

修改前必须确认：

* `frame.markers` 中的 `isSaved` 在点位完成后能够及时刷新。
* marker completion changed 后会触发下一次 marker snapshot 更新。
* 云同步完成状态改变后也会刷新对应 snapshot。
* 不允许因此出现“玩家完成点位后，小地图标记长时间不消失”的问题。

如果现有刷新机制不足，只允许补充**最小范围的 marker snapshot invalidation / refresh**。

不要把完成状态查询重新放回 Render Thread。

---

# 4. 第二阶段：优化标记纹理查找

重点文件：

`IMao-Core/src/ImguiDraw/Items/DrawMarkerInteraction.cpp`

`IMao-Core/src/ImguiDraw/Items/DrawItemBase.h`

当前 `DrawIcon()` 中存在：

```cpp
for (const auto& cached : DrawItemBase::itemsTextureData)
    if (cached.nameId == item.nameId) {
        texture = cached.texture;
        break;
    }
```

当前纹理容器为线性结构时，每个 marker 每帧都可能进行一次线性搜索。

## 修改目标

增加 O(1) 或接近 O(1) 的纹理索引。

允许两种方案：

### 方案 A：保留现有 vector，同时增加索引

例如：

```cpp
std::unordered_map<std::string, size_t> itemTextureIndex;
```

通过 nameId 查找 vector 下标。

### 方案 B：直接使用 unordered_map

例如：

```cpp
std::unordered_map<std::string, ItemTextureData>
```

但只有在不会大量影响其他调用方时采用。

## 优先原则

优先选择**改动范围最小**的方案。

禁止为了这一个查找问题重构整个资源系统。

## 注意

设备重建时现有代码会：

```cpp
DrawItemBase::itemsTextureData.clear();
```

新增索引必须同步清空，否则可能留下失效 SRV。

---

# 5. 第三阶段：增加 Marker Render 性能诊断

在改变 marker 渲染方案之前，先加入低开销诊断。

建议在现有：

`overlay-motion`

日志体系中增加 marker 相关统计。

至少记录：

```text
markerCount=
markerGroupCount=
markerDrawMs=
markerTextureLookupMs=
markerLayoutMs=
```

其中：

* `markerCount`：原始候选 marker 数量
* `markerGroupCount`：`BuildMarkerLayout()` 后实际绘制 icon 数量
* `markerLayoutMs`：布局计算耗时
* `markerTextureLookupMs`：纹理获取总耗时
* `markerDrawMs`：整个 `DrawItemsOnMinMap()` 的绘制准备耗时

不要每帧写日志。

继续沿用现有每 2 秒聚合输出方式。

禁止因为性能诊断本身产生明显性能损耗。

---

# 6. 第四阶段：验证真正的小尺寸 MiniMap Overlay

这是本轮最重要的实验。

当前普通探索状态下，只需要在游戏左上角小地图附近绘制 marker，但 Overlay 本身仍接近整个游戏窗口大小。

必须验证：

**真正的小尺寸 HWND + 小尺寸 SwapChain 是否比当前全屏 Overlay 显著便宜。**

注意：

这和以前测试“在全屏 Overlay 里面只画 1×1 或 450×70 色块”不是同一个实验。

以前测试改变的是：

`绘制面积`

本次需要改变的是：

`实际 HWND 尺寸 + 实际 SwapChain / BackBuffer 尺寸`

---

# 7. MiniMap Overlay 实验版本设计

暂时不要直接重构生产架构。

先制作一个可切换的实验模式。

建议增加一个诊断或 isolation 开关，例如：

```text
mini-overlay-experiment
```

或者沿用现有 IsolationSwitches。

实验模式仅在：

```text
小地图存在
大地图未打开
小地图点位显示开启
```

时启用。

## 模式 A：现有行为

```text
Overlay HWND：
与游戏客户区同尺寸

SwapChain：
与游戏客户区同尺寸
```

## 模式 B：MiniMap Overlay

计算实际小地图矩形：

```text
minimapRect
```

然后向四周增加约：

```text
30～60 px padding
```

用于：

* marker 超出小地图圆形边缘的部分
* 数字角标
* 当前目标提示等小型元素

最终：

```text
MiniOverlayRect =
MinimapRect + Padding
```

例如实际可能是：

```text
350×350
或
400×400
```

而不是：

```text
2560×1440
```

实验模式下：

```text
HWND 物理尺寸 = MiniOverlayRect
SwapChain Buffer = MiniOverlayRect
```

marker 坐标需要转换为这个局部 Overlay 坐标：

```cpp
localX = screenX - overlayLeft;
localY = screenY - overlayTop;
```

---

# 8. MiniMap Overlay 实验的重要约束

必须保证：

### 坐标行为不变

marker 在游戏画面上的最终物理位置必须和原来一致。

### 小地图运动跟踪不变

`ImageAnchoredOverlay`

`OverlayScreenTransform`

以及现有 minimap motion tracking 的语义不允许改变。

只改变最终绘制坐标系：

```text
游戏客户区坐标
→
MiniOverlay 局部坐标
```

### 点击行为不变

普通小地图 Overlay 本身原则上应继续完全鼠标穿透。

### DPI 不回归

至少保证：

* 100%
* 125%
* 150%

不会因为局部 HWND 而产生坐标偏移。

### 窗口移动

游戏窗口移动后 MiniMap Overlay 必须跟随。

但不要引入新的高频 `SetWindowPos` 调用。

仍应保持：

```text
位置没有变化
→
不调用 SetWindowPos
```

---

# 9. 大地图状态暂时不要重构

本轮实验只针对：

```text
普通游戏状态
+
小地图 marker
```

当大地图打开时：

继续使用当前 FullScreen Overlay。

不要本轮顺手重构大地图 Overlay。

目标架构暂时可以理解为：

```text
普通探索：
MiniMap Overlay

打开大地图：
FullScreen Overlay
```

但正式切换架构必须等实验数据证明有效以后再进行。

---

# 10. 第五阶段：纹理 Atlas 仅做设计与可选原型

当前 marker 绘制大致为：

```cpp
AddCircleFilled(...)
AddImageRounded(...)
AddCircle(...)
```

即每个 marker 需要多个 primitive。

同时不同 marker 可能使用不同 SRV。

如果前面的小优化完成后 marker 绘制仍然明显昂贵，再进行 Texture Atlas 原型。

目标：

把：

```text
背景圆
图标
边框
```

预先烘焙或合并到 Atlas 中。

运行时每个普通小地图 marker 尽量变成：

```cpp
AddImage(...)
```

一个 Quad。

所有 marker 共用：

```text
1 个 Atlas SRV
```

仅在：

```text
group.members.size() > 1
```

时额外绘制数字角标。

## 本轮限制

如果 MiniMap Overlay 实验已经可以把掉帧控制到合理水平，本轮不强制实现 Atlas。

禁止为了 Atlas 重写整个 marker 系统。

---

# 11. 第六阶段：WGC ROI Readback 作为独立后续实验

当前 WGC 已经经过：

* MinUpdateInterval
* 缓冲复用
* scratch buffer
* 非必要分配削减

普通探索状态下真正需要识别的区域主要是：

```text
小地图
HUD 探针区域
```

后续可以测试：

使用：

```cpp
CopySubresourceRegion(...)
```

将源 capture texture 的局部区域复制到较小 staging texture。

避免普通探索状态持续进行整张：

```text
2560 × 1440 × 4
```

GPU→CPU readback。

但是：

**不要和 MiniMap Overlay 实验混在同一个 commit 中。**

否则性能收益无法归因。

---

# 12. 性能测试要求

必须使用同一次运行内的交替 A/B 测试。

禁止：

```text
今天测 A
明天测 B
```

也禁止：

```text
先跑一分钟 A
然后只跑一次 B
```

建议：

```text
A → B → A → B
```

或者：

```text
B → A → B → A
```

每段：

```text
30～45 秒
```

切换后给予：

```text
5～10 秒
```

稳定时间，该时间不纳入统计。

游戏人物尽量保持：

* 同一个位置
* 同一个视角
* 同一个场景
* 不进行战斗
* 不打开菜单
* 不快速转镜头

---

# 13. 必须记录的性能指标

PresentMon 至少统计：

```text
FPS
p50 frame time
p95 frame time
p99 frame time
>20ms frame ratio
PresentMode
```

工具侧同时记录：

```text
overlay-motion
capture-cadence
capture-wgc-frames
markerCount
markerGroupCount
markerDrawMs
markerLayoutMs
```

如果测试 MiniMap Overlay，还需要记录：

```text
overlayWidth
overlayHeight
backBufferWidth
backBufferHeight
overlayMode=full|minimap
```

---

# 14. 成功判定

本轮不要求达到“完全零掉帧”。

MiniMap Overlay 实验满足以下任一情况，即认为值得正式落地：

### 情况 A

相同游戏场景中：

```text
平均 FPS 恢复 ≥ 5 FPS
```

并且 A/B/A/B 趋势一致。

### 情况 B

平均 FPS 提升不大，但：

```text
>20ms frame ratio
```

下降至少 50%。

### 情况 C

p95 / p99 帧时间明显下降，玩家体感稳定性明显改善。

如果：

```text
Full Overlay
和
MiniMap Overlay
```

在交替实验中基本没有稳定差异，则不要继续为了“小窗口”进行正式架构重构。

记录实验结论后停止该方向。

---

# 15. 每阶段提交要求

禁止把所有改动堆在一个 commit。

建议至少拆为：

```text
perf: use marker snapshot completion state during minimap rendering

perf: add constant-time marker texture lookup

diag: add minimap marker rendering timing metrics

experiment: add real small minimap overlay presentation mode

perf: implement minimap texture atlas
```

最后一项只有确认需要 Atlas 时才做。

每个 commit 必须：

* 能独立编译
* 原有测试通过
* 不包含无关格式化
* 不包含无关重命名
* 不修改不相关文件

---

# 16. 回归检查

至少检查：

1. 小地图 marker 位置正确。
2. marker 不闪烁。
3. marker 完成后正常消失。
4. 云同步完成状态变化后 marker 正常刷新。
5. 重叠 marker 数字角标正常。
6. 大地图 marker 不受影响。
7. 路线绘制不受影响。
8. 小地图定位失败/恢复行为不变。
9. 切换大地图后 Overlay 正常切换。
10. 游戏窗口移动后 Overlay 位置正确。
11. DPI 100% / 125% / 150% 坐标正确。
12. Alt-Tab 后正常恢复。
13. 最小化/恢复后正常。
14. Overlay 仍然鼠标穿透。
15. 游戏关闭/重新打开后没有残留 Overlay HWND。
16. D3D device recreation 后 texture cache / index 均正确重建。

---

# 17. 明确禁止事项

本轮禁止：

* 重写整个 ImGui Overlay。
* 更换 GUI 框架。
* 把所有 Overlay 迁到 Direct2D。
* 为了性能将所有代码改成 ECS。
* 新建大量 manager/service/controller。
* 修改地图定位数学模型。
* 修改视觉定位阈值。
* 修改 SIFT/SURF/OpenCV 算法。
* 重写 MarkerCompletionStore。
* 修改 IPC 协议，除非诊断开关确实需要极小字段。
* 默认启用未经验证的 DirectComposition。
* 删除当前 layered fallback。
* 修改现有 Release 行为，除非实验结果已经完成验证。
* 根据一次 FPS 截图直接宣称性能问题已解决。

---

# 18. 执行顺序

严格按照：

```text
Phase 1
Render Thread 不再实时查询 completion
↓
测试

Phase 2
纹理查找 O(1)
↓
测试

Phase 3
补 marker 分段性能日志
↓
建立新基线

Phase 4
实现真正的小尺寸 MiniMap Overlay 实验模式
↓
A/B/A/B PresentMon 实测

有效：
正式设计 Full Overlay / Mini Overlay 切换

无效：
停止该方向

之后再决定：
Texture Atlas
或
WGC ROI Readback
```

不要同时推进三个大方向。

---

# 19. 最终交付报告

完成后请输出一份简洁报告，必须包含：

```text
1. 修改了哪些文件
2. 每个文件为什么修改
3. 新增了哪些性能诊断
4. 修改前基线
5. 修改后数据
6. A/B/A/B 每一段的 FPS / p95 / p99 / >20ms
7. 是否确认 MiniMap Overlay 有收益
8. 是否存在功能回归
9. 仍未解决的性能问题
10. 下一步建议
```

对于没有实际测量的数据必须明确写：

```text
未验证
```

禁止写成：

```text
预计已解决
应该已经优化
理论上不会掉帧
```

---

## 本轮最终目标

不是“让代码看起来更先进”。

而是尽可能回答这个具体问题：

> 当游戏只需要在小地图附近显示几个点位时，能否避免为了这些 marker 长期维护和绘制一个整个游戏客户区大小的 Overlay，从而显著降低 IMAO 对游戏帧率和帧时间稳定性的影响？

先用可靠实验回答这个问题，再决定是否进行正式架构调整。

---

# 20. 开工前的现状核实（2026-09-21，读代码取到的证据）

任务书点名的位置**与当前代码一致**，无需重新定位。以下每条都注明「文件 + 函数名」，行号是当时的值（会漂移）。

## 20.1 任务书点名的三处仍然存在

| 任务书的说法 | 现状 | 证据 |
|---|---|---|
| `DrawItemsOnMinMap()` 里有 `IsPointCompleted` 实时查询 | ✅ 仍在，且是**每 marker 一次** | `DrawItemOnMinMap.cpp:210`（`DrawItemOnMinMap`） |
| `DrawIcon()` 里线性搜索纹理 | ✅ 仍在 | `DrawMarkerInteraction.cpp:1031`（`DrawIcon`） |
| 设备重建时 `itemsTextureData.clear()` | ✅ 仍在 | `ImGuiOverWindows.cpp:1056` |

## 20.2 Phase 1 的前置条件（任务书 §3「必须确认」）

**结论：`isSaved` 的刷新机制已经足够，Phase 1 不需要新增任何 invalidation 逻辑。** 证据链：

1. **填充点**：`DrawItemOnMinMap::GetAndFilterItemsData`（`DrawItemOnMinMap.cpp:133-142`）在遍历候选时
   用 `DrawItemBase::GetFilteredPoints(senceName, nameId)` 取该组的已完成集合，逐点写入
   `tempItemData.isSaved`。`GetFilteredPoints` → `MarkerCompletionStore::CompletedIds`（含云端 `remoteIds`）
   ⟹ **本地完成与云同步完成都走同一条路**，两者都会反映到 `isSaved`。
2. **谁在什么时候填**：`App::Start()`（`App.cpp:266`，`co_await winrt::resume_background()` 的采集消费线程）
   每消费一帧采集就调用 `DrawItemOnMinMap::UpdatePlayerNearItemsData(rect, ...)`（`App.cpp:523-524`）。
3. **冻结进 frame**：**同一线程、同一轮循环**里，稍后 `PublishOverlayFrame`（`App.cpp:2490`，
   由 `App.cpp:557` 调用）在 `App.cpp:2538` 执行 `frame.minimapMarkers = DrawItemOnMinMap::Snapshot();`
   ——即 `isSaved` 在**同一轮、发布之前**刚从 store 读出，不是陈旧值。
4. **谁在读**：渲染线程在 `ImGuiOverWindows.cpp:710` 用 `frame->minimapMarkers` 调 `DrawItemsOnMinMap`。
5. **已有先例**：`NearbySelection` 早就把 `item.isSaved` 当作完成态使用
   （`NearbySelection.h:60/118/146`），说明"frame 里的 `isSaved` = 完成态"已是既有的、被信任的约定。

**因此"完成后标记长时间不消失"的风险上界 = 一个采集周期**：完成动作发生后，下一轮
`UpdatePlayerNearItemsData` 就会把 `isSaved` 置真，下一份 frame 发布后渲染侧即跳过该 marker。
任务书担心的"长时间不消失"不成立（除非采集停摆，而那种情况下位置本身也已经不刷新）。

### 顺带发现（留给 Phase 3 量化，本阶段不动）

* `MarkerCompletionStore::Completed()` 取的是 `std::scoped_lock mutex`，而**写路径 `Execute()` → `Commit()` 是在持锁状态下
   写文件**（`MarkerCompletionStore.h` 是 header-only，`Commit` 在该文件末尾）。所以渲染线程原来这条查询不只是加锁，
   还可能**阻塞在磁盘写**上——这正是"帧时间长尾"的合理候选机制之一。Phase 1 已把它从渲染路径整体移走。
* `App.cpp:2544-2545` 在发布路径上**又对每个 marker 查了一次** `IsPointCompleted`（为游戏手柄上下文刷新
  `isSaved`）。它在采集线程、不在渲染线程，**本轮不动**（属任务书 §17「不修改不相关文件」范围之外），
  但记录在此：Phase 1 之后它是这条链路上最后剩下的每帧逐 marker 查询。

## 20.3 Phase 2 的现状

`DrawIcon`（`DrawMarkerInteraction.cpp:1031`）按 `nameId` 线性搜索 `DrawItemBase::itemsTextureData`
（`std::vector<ItemTextureData>`，`DrawItemBase.h:63`），命中则在 `:1041` 追加。调用方只有这一处查找，
采用任务书的**方案 A（保留 vector + 增加索引）**改动面最小。

## 20.4 不能自己测的部分

任务书 §12/§13 的 PresentMon A/B 测量需要**玩家本人**在真实游戏里按同一位置/视角跑
`A → B → A → B`（每段 30～45 秒）。工具侧只能保证诊断日志齐备；**FPS / p95 / p99 / >20ms 的数字
必须由实机测量产生，未测之前一律记为「未验证」。**

---

# 21. 进度台账

| 阶段 | 内容 | 状态 | commit | 编译/测试证据 | 性能实测 |
|---|---|---|---|---|---|
| Phase 0 | 开分支 + 任务书落档 | 已完成 | `690dc22` | 不适用 | 不适用 |
| Phase 1 | 渲染线程不再实时查询 completion | 已实现 · **实机判定通过**（§25.1） | `44ed966` | ✅ 见 §23 | 行为已验证；收益**未验证**（需同场 A/B） |
| Phase 2 | 纹理查找 O(1) | 已实现 · 命中成本已实测（§25.2） | `095d129` | ✅ 见 §23 | 命中 1–2 µs/图标；**省下多少未验证** |
| Phase 3 | marker 分段性能日志 | 已实现 · **已产出真实数据**（§25.3） | `8cf0a91` | ✅ 见 §23 | 字段语义见 §24.4 |
| Phase 4 | 真正的小尺寸 MiniMap Overlay 实验模式 + A/B/A/B | **已实测：有明确收益**（§28） | `b5b6fb3`（脚本 `b2e7fbf`，UI `f4f1d8e`） | ✅ 见 §23 | **+9.6 fps / p95 −2.4 ms / >20ms→0**（本场景） |
| Phase 5 | Texture Atlas | 未开始（**取决于 Phase 4 结论**） | — | — | 未验证 |
| Phase 6 | WGC ROI Readback | 未开始（**独立后续实验**） | — | — | 未验证 |

> 附：Phase 3 的一个收尾改动 `b4f70e4`（`markerSkippedCompleted` 计数，见 §25.6）已一并落地，尚未实测。

## 回归检查（任务书 §16，逐项待实测）

16 项全部**未验证**。Phase 1 相关的第 3、4、6 项优先，Phase 4 相关的第 9～15 项在实验模式落地后测。

---

# 22. 构建与测试环境（2026-09-21 本会话实测，给下一次会话省时间）

| 项 | 结论 | 证据 |
|---|---|---|
| Ninja 构建树**在本会话不可用** | `out/build/windows-x64-release` 的生成边一启动就停住，ninja/cmake 进程 CPU 恒为 0，不推进也不报错 | 决定性实验：`out/perf/ninja-spawn-test` 里一个**只跑 `cmd /c echo`** 的极小 ninja 工程同样挂起（>60s 无输出）⟹ 本沙箱下 ninja **无法启动任何子进程**，与工程本身无关 |
| **可用路径**：VS 生成器树 + MSBuild | `cmake --build out\build\windows-x64-release-vs144 --config Release --target …` | 先用极小工程 `out/perf/msbuild-spawn-test` 验证：configure 10.2s、build 6.4s，cl.exe 正常 |
| 两个树**共用输出目录** | 都输出到 `x64\Release` | ninja 的 `build.ninja:1702` 与 VS 树同样设置 `RUNTIME_OUTPUT_DIRECTORY` |
| `out/build/windows-x64-release/build.ninja` 的 mtime 状态 | 该文件是生成物、不进 git；排查期间我改过它两处并已在事后**逐字还原**（`cmake.verify_globs` 的 force 输入、`build.ninja` 的自重生成输入） | `git status` 看不到它；还原后两行与 `rules.ninja` 生成的原文一致 |
| `CMakeFiles/cmake.verify_globs` 时间戳被触碰过一次 | 只影响那个不可用的 ninja 树；glob 集合**没有变化**（单独跑 `VerifyGlobs.cmake` exit 0、无 `GLOB mismatch`） | `cmake -P …/VerifyGlobs.cmake` = 0.2s，无输出 |

**下次要注意**：`scripts/Test-Runtime.ps1` 走的是 Ninja 树，若沙箱仍禁止 ninja 起子进程，它会卡在 `[0/2] Re-checking globbed directories...`。可先用 §22 的 MSBuild 路径验证编译，再单独跑测试 exe。

---

# 23. 本会话的编译/测试证据（Phase 1 + Phase 2）

命令（全程同一棵 VS 生成器树，Release）：

```text
cmake --build out\build\windows-x64-release-vs144 --config Release \
    --target IMao-CoreHost IMaoOptimizationTests IMaoMarkerTests --parallel 8
```

| 检查 | 结果 |
|---|---|
| 错误数（`error C`/`error LNK`/`error MSB`） | **0**（Phase 1 与 Phase 2 两次构建都是 0） |
| 警告 | Phase 1: 209 条（均为既有的 C4244 等，与本次改动无关） |
| Phase 1 产物 | `IMao-CoreHost.exe` 20:26:36、`IMaoOptimizationTests.exe` 20:28:49 |
| Phase 2 产物 | `IMao-CoreHost.exe` 20:34:04；三个改动 TU（`DrawMarkerInteraction.cpp`/`ImGuiOverWindows.cpp`/`DrawItemBase.cpp`）在日志中各有一次编译记录 |
| Phase 3 产物 | `IMao-CoreHost.exe` 20:58:19（`sha8=F829F061`）；`DrawItemOnMinMap.cpp`/`ImGuiOverWindows.cpp`/`DrawMarkerInteraction.cpp` 各有一次编译记录 |
| `IMaoOptimizationTests.exe` | exit 0，末行 `All optimization tests passed.` |
| `IMaoMarkerTests.exe` | exit 0，末行 `Marker layout, interaction, account isolation and durable synchronization tests passed` |
| 测试数据隔离 | 测试运行时 `LOCALAPPDATA` 指向 `out\perf\local-app-data`，不触碰玩家真实数据 |

⚠️ **这两个测试目标不含 ImguiDraw 的源文件**（`CMakeLists.txt` 里 `IMaoOptimizationTests` 是显式源列表），所以它们**不能**证明 overlay 绘制路径的行为；它们只能证明"没编译坏"。真正验证 marker 行为需要实机回归（任务书 §16）。

⚠️ **FPS / p95 / p99 / >20ms / PresentMode：全部未验证**——需要玩家在同一次运行里做交替 A/B，见任务书 §12/§13。

## 为什么最终连 Phase 3 也做了

任务书 §18 的顺序是 `Phase 1 → 测试 → Phase 2 → 测试 → Phase 3`。实机测试在 2026-09-21 晚做了一轮，但那一轮**测的是改动前的构建**（见 §24.2），而且它暴露了一个更根本的问题：**当时的日志在原理上就判不出 Phase 1 的效果**——日志记的是候选集，而候选集包含已完成的点（见 §24.1）。
所以先补 Phase 3 的诊断，让"完成后标记消失"这件事第一次在日志里可判，再用一次实机会话同时验证 Phase 1/2/3。

---

# 24. 诊断字段语义与实机基线（Phase 3 相关）

## 24.1 为什么原来的日志判不出"完成后标记消失"

`DrawItemOnMinMap::GetAndFilterItemsData` 对已完成的点**只打标记不剔除**：命中距离窗口的点一律进 `nearFilterItemsData`，已完成的那一个带 `isSaved=true`。剔除发生在渲染那一刻（`DrawItemsOnMinMap`）。而所有 marker 日志用的都是这个集合：

| 日志 | 记录的东西 |
|---|---|
| `minimap-near-items markers=` | 候选集大小（**含已完成**） |
| `minimap-marker-sample markers=/samples=` | 候选集及其逐点坐标 |
| `RuntimeStatus::SetMinimapMarkerCount` → 游戏内状态栏"N 个标记" | 候选集大小（**含已完成**，既有行为） |

⟹ 基线数据正好印证：2026-09-21 20:46:43 完成一个点位之后，`markers=` 从 2 **没有下降**。
⟹ 所以"完成后标记是否立刻消失"这件事，**改动前后都必须靠 `markerCount`（候选）与 `markerGroupCount`（真正画出的 icon）之差**来看，肉眼观察不足为证。

## 24.2 实机基线会话（改动前构建，2026-09-21 20:44:36–20:51:00）

**重要：这一轮跑的不是本次改动。** 日志里 `resource-load` 的路径暴露了实际运行位置是 `C:\Dapps\IMao`，其 `IMao-CoreHost.exe` 当时是 18:33 的构建（`sha8=45C99B91`），而本次 Phase 1+2 的构建是 `8655FBB9`。因此这一段是**纯粹的改动前基线**，可用作对照。

| 观察 | 数值 |
|---|---|
| 会话边界 | CoreHost 20:44:36 启动 → 20:51:00 停止；`app-init client=2560x1440` |
| Overlay 窗口 | `origin=0,0 size=2560x1440`、`mode=layered-colorkey`（全屏分层窗口，Phase 4 尚未做） |
| 完成动作 | **3 次**：20:46:43 / 20:49:40 / 20:49:43（`已完成当前附近点位。`） |
| 大地图开合 | 7 次（`map-ui-transition`） |
| `minimap-overlay-drop` | **0**（没有因配准失败整层不画） |
| 崩溃 | 本次会话内 0 次（但当天另有两次 dump，见 §24.3） |
| 工具自身节奏（**不是游戏 FPS**） | `renderFps` 中位 29.7；`sourceFps` 中位 10.9；`captureFps` 中位 17.7；`segBuildMs` 中位 0.75 / 最大 7.1；`segPresentMs` 中位 0.67 / **最大 20.9 ms**；`presentSkipped` 合计 3286 |
| `minimap-markers-cleared` | 10 次，全部 `state=Uninitialized stalling=1`，且几乎都紧跟关闭大地图之后 ⟹ 与已知的"整帧丢位置"行为一致，不是新回归 |
| 候选集规模 | `markers=` 在 1～5 之间（20:46:08 起 1 → 2 → 4 → 5 → 4…） |

`segPresentMs` 最大 20.9 ms 这条长尾与既有分析（全屏置顶分层窗口把游戏压回 DWM 合成）一致，可作为 Phase 4 的对照。

## 24.3 当天两次 CoreHost 崩溃（与本次改动无关）

`%LOCALAPPDATA%\IMao-WinUI\CrashReports\` 里有 `IMao-Core-2026-9-21-19-40-27.dmp`（587 KB）与 `IMao-Core-2026-9-21-1-31-32.dmp`（573 KB）。两次都发生在**改动前**的构建上（本次构建 20:26 之后才存在），日志在 19:40:27 前后也没有对应事件。**未分析**，需要时另开一轮看 minidump。

## 24.4 新字段的确切语义（`overlay-motion` 行尾）

沿用该行既有的 `seg*Ms = 总时长/次数` 约定：

```text
markerCount=            候选 marker 数的**每次绘制平均**（含已完成的点）
markerGroupCount=       BuildMarkerLayout 之后真正绘制的 icon 数**每次绘制平均**
markerDrawMs=           DrawItemsOnMinMap 整函数耗时 / markerDraws（含下面的 layout 与 texture）
markerLayoutMs=         BuildMarkerLayout 耗时 / markerDraws
markerTextureLookupMs=  DrawIcon 里"查索引 + 必要时解码加载"耗时 / markerDraws
markerDraws=            该 2 秒窗口内 DrawItemsOnMinMap 的调用次数
```

- **判定 Phase 1 的方法**：在同一次 2 秒窗口内，`markerCount` 不降而 `markerGroupCount` 降 ⟹ 被完成掉的点当帧就不再绘制。若两者同步下降，说明它其实是离开了候选集（位置走远），不是完成生效。
- `markerTextureLookupMs` 只含**小地图自己**那几次 `DrawIcon`：实现用的是"单调累加 + 调用方取差值"，所以大地图共用 `DrawIcon` 不会污染这个数。
- 计时开销：每个 icon 两次 `steady_clock::now()`（≈QueryPerformanceCounter）。大地图几百个 icon 的最坏情况约数百次调用/帧，相对既有 `seg*` 计时可忽略；**尚未在实机上对比过开关前后的 `renderFps`**。
- ⚠️ 这些数字**全部未验证**——直到有实机会话跑出带这些字段的 `overlay-motion` 行为止。

## 24.5 部署状态（2026-09-21 21:0x）

| 位置 | 内容 |
|---|---|
| `C:\Dapps\IMao\IMao-CoreHost.exe` | `F829F061`（20:58，Phase 1+2+3）——**这是玩家实际启动的那一份** |
| `C:\Dapps\IMao\IMao-CoreHost.before-perf-phase12-20260921.exe` | `45C99B91`（18:33，改动前，保留作回滚/A 组） |
| `C:\Dcode\WWMAP-TOOLS\x64\Release\IMao-CoreHost.exe`、`out\map-test\IMao-CoreHost.exe` | 同上 `F829F061` |

⚠️ **回滚**：`Copy-Item 'C:\Dapps\IMao\IMao-CoreHost.before-perf-phase12-20260921.exe' 'C:\Dapps\IMao\IMao-CoreHost.exe' -Force`。
⚠️ 以后换构建时，**记得换的是 `C:\Dapps\IMao` 这一份**，仓库里的两份不会影响实机（这一点已经浪费过一轮测试）。

---

# 25. 实机验证结果（2026-09-21 21:00:08–21:04:39，构建 `F829F061`）

**构建指纹先立住**：这一窗口内 `overlay-motion` 共 95 行，其中 **95 行都带 `markerCount=`/`markerGroupCount=`**——这两个字段只存在于 `F829F061`，所以这次跑的确实是 Phase 1+2+3 的构建（不再靠推断）。

场景：`app-init client=2560x1440`；Overlay 仍为 `origin=0,0 size=2560x1440` `layered-colorkey`（Phase 4 尚未做）；`Overlay` 的设备在本次会话中**只创建过一次**（`overlay-presentation` 1 行）。

## 25.1 Phase 1：**判定通过**（候选集保留 + 绘制数 −1，带点位 id 证据）

本次共 4 次完成动作（`nearby-*` 与通知）：

| 时刻 | 事件 |
|---|---|
| 21:01:41 | `nearby-collect-all revision=14 completed=2 skipped=0 failed=0`（一键收集 2 个） |
| 21:01:48 | `nearby-result complete-single point=8:1322600542278930432 accepted=1` |
| 21:02:19 | `nearby-result complete-single point=8:1322590878473842688 accepted=1` |
| 21:04:19 | `nearby-result complete-single point=8:1325195116886908928 accepted=1` |

**证据一：完成的点位仍留在候选集里（这是 Phase 1 的核心语义）。**
`minimap-marker-sample` 逐点记 `itemId`。21:02:19 完成的 `1322590878473842688` 在完成**之后**继续出现在候选集里（21:02:20 → 21:02:56，以及 21:03:55 → 21:04:34，直到会话结束）；21:01:48 完成的 `1322600542278930432` 同样继续出现到 21:02:14 才因玩家走远而离开窗口。
⟹ 已完成 ≠ 离开候选集，与"渲染时按 `isSaved` 跳过"的设计一致。

**证据二：同一时刻绘制的 icon 数恰好 −1，而候选数不变。**

| 时刻 | `markerCount` | `markerGroupCount` | markerDraws |
|---|---|---|---|
| 21:02:17 | 3.65 | 3.00 | 60 |
| 21:02:19（完成 #2） | 3.00 | 3.00 | 60 |
| 21:02:21 | 3.00 | **2.25** | 60 |
| 21:02:23 | 3.00 | **2.00** | 60 |
| 21:02:25 | 3.00 | 2.00 | 60 |

**证据三（最强旁证）：21:01:51–21:01:59 连续 8 秒 `markerCount=3.00` 而 `markerGroupCount=0.00`。**
那 3 个候选正好是"21:01:41 一键收集的 2 个 + 21:01:48 完成的 1 个"——**全部是已完成点位，因此一个都没画**。候选集非空却一个 icon 都不画，只能解释为完成态被读到并跳过。

⟹ 任务书 §16 第 3 项（完成后标记正常消失）**通过**，且不是靠"整帧丢位置"造成的假象（同段 `minimap-markers-cleared` 只在 21:01:21 出现一次，属冷启动）。

## 25.2 Phase 2：索引命中成本 = 1～2 µs/图标

| 窗口 | `markerTextureLookupMs`（每次绘制平均） | 说明 |
|---|---|---|
| 21:01:22 | **0.7974 ms** | 冷启动，该窗 42 次绘制里含 icon 首次解码（合计约 33 ms） |
| 21:01:24 – 21:02:07 | **0.0010 – 0.0019 ms** | 索引命中 |
| 21:02:09 | 0.0350 ms | 新 icon 首次加载 |
| 21:02:25 / 21:04:10 | 0.0107 / 0.0016 ms | 新 icon / 命中 |

⟹ 命中索引约 **1–2 µs/图标**。⚠️ 注意本条只说明"现在有多便宜"，**不能**据此宣称"省了多少"——线性扫描的对照值本次没有测（要做就得用改动前构建做同场交替 A/B）。在本次 1～6 个候选的规模下，两种实现的差距本来就落在噪声里；O(1) 索引主要服务于大地图那种几十上百个图标的情形，而**大地图路径本次没有单独计时**。

## 25.3 marker 绘制到底有多贵（对 Phase 4 的判断依据）

| 指标 | 本次实测 |
|---|---|
| `markerDrawMs`（`DrawItemsOnMinMap` 整函数/每次绘制） | 0.015 – 0.031 ms（绘制 0–3 个图标时），首窗 0.823 ms（含冷解码） |
| `markerLayoutMs`（`BuildMarkerLayout`/每次绘制） | 0.002 – 0.007 ms |
| 同期 `segBuildMs`（2 秒窗口内每次渲染平均） | 中位 **0.81 ms** |
| 同期 `segPresentMs` | 中位 0.73 ms、**最大 23.2 ms** |

⟹ **marker 的 CPU 绘制总耗时是几十微秒/帧，约占 build 段的 2–4%**。而同一会话里 present 的长尾是 **23 ms 量级**。
⟹ 这直接支持任务书把 Phase 4 当成本轮核心实验：**帧率问题的量级不在 marker 绘制里，而在"整个游戏客户区大小的窗口 + SwapChain 的 present/合成"里**。

## 25.4 回归检查（本轮能判的都判了）

| §16 项 | 结果 | 依据 |
|---|---|---|
| 2 marker 不闪烁 | 通过 | `markerCount` 序列没有 0↔N 反复跳变；所有 `markerGroupCount=0` 的窗口都能归因到开大地图/失焦/恢复 |
| 3 完成后正常消失 | **通过** | §25.1 |
| 6 大地图 marker 不受影响 | 通过 | 21:02:27 / 21:02:47 / 21:03:58 三次开图均有 `map-viewport-result`，关图后小地图 marker 正常恢复 |
| 9 切换大地图后 Overlay 正常 | 通过 | `overlay-window-visibility` 有 `idle` 隐藏 → `content` 显示的完整往返（21:02:43/21:02:48/21:02:57/21:03:54） |
| 12/13 Alt-Tab / 最小化后恢复 | 通过（间接） | 21:02:57–21:03:54 有 57 秒 `state=Unknown focused=0` + `visible=0 reason=idle`，回来后 21:03:55 起 marker 正常重画 |
| 崩溃 / `minimap-overlay-drop` | 0 次 / 0 次 | 本会话日志 |
| 4 云同步完成状态刷新 | **未验证** | 本轮没有做同步动作 |
| 11 DPI 100/125/150 | **未验证** | 只有 `dpi-awareness per-monitor-v2` 一条，本轮只跑了一种 DPI |
| 14 鼠标穿透 / 15 无残留 HWND | **未验证** | 日志判不了 |
| 16 D3D device recreation 后索引重建 | **未验证** | 本会话 `overlay-presentation` 只出现 1 次 ⟹ **没有发生设备重建**，`itemTextureIndex.clear()` 那条路一次都没走到 |

## 25.5 本轮**不给**任何性能结论

与 §24.2 基线（改动前构建）对比：`renderFps` 29.72 → 29.71、`sourceFps` 10.87 → 10.87、`segBuildMs` 中位 0.75 → 0.81、`segPresentMs` 最大 20.9 → 23.2。
**这是跨会话、跨场景的比较，按任务书 §12 不能作为性能结论**；它只能说明"加了诊断后各项量级没变"。游戏 FPS / p50 / p95 / p99 / >20ms frame ratio / PresentMode：**全部未验证**，需要 Phase 4 的同场交替 A/B。

# 26. Phase 4：小尺寸 MiniMap Overlay 实验模式（已实现，**仅编译验证**）

## 26.1 怎么开、怎么关

| 动作 | 做法 |
|---|---|
| 打开 | 工具 → 诊断页 → **隔离开关** 框里填 **`128`** |
| 关闭 | 同一个框里填 **`0`** |
| 生效条件 | `frame.minimapVisible && !frame.mapVisible`：小地图存在、大地图未打开、小地图点位显示已开 |
| 开大地图时 | 自动回到整屏窗口（视口匹配与大地图 marker 保持现状，本轮不动大地图） |

开关本身是 `IMao-Core/src/Runtime/IsolationSwitches.h` 的 **bit 7（`kMiniOverlay`）**，日志里名为 `mini-overlay`。诊断页那个 NumberBox 直接把掩码发给 CoreHost（`CoreHostMain.cpp` 的 `setIsolationSwitches`），所以没有新增 IPC。

⚠️ **那个框原来上限是 127，所以 128 根本填不进去**（2026-09-21 实测发现）。已由 `f4f1d8e` 把 `DiagnosticsPage.xaml` 的上限改成 255，并把 128 写进说明文字。
⚠️ 改动落在**托管外壳**（`resources.pri` 里编译过的 XAML），所以：**先完全退出 IMAO（含托盘），再启动**，否则你看到的还是旧的 127 上限。
⚠️ 填完值后要**按 Tab 或点别处**让输入生效（NumberBox 在值变化时才发送）。

### 26.1.1 托管外壳怎么部署的（含本环境的已知限制）

托管 app 的源码与发布版（`8fa437c`）**逐字节一致**（`git diff 8fa437c..HEAD -- IMao-WinUI IMao-WinUI.Core` 为空），我只改了 XAML。重建命令（照 `Build-ReleaseCandidate.ps1`）：

```powershell
. .\scripts\Enter-DevEnvironment.ps1
& $env:IMAO_DOTNET build   IMao-WinUI/IMao-WinUI.csproj -c Release -r win-x64 --self-contained true -p:NuGetAudit=false --source $env:NUGET_PACKAGES
& $env:IMAO_DOTNET publish IMao-WinUI/IMao-WinUI.csproj -c Release -r win-x64 --self-contained true -p:NuGetAudit=false --source $env:NUGET_PACKAGES --no-restore -o <输出目录>
```

⚠️ **本环境里这条命令 exit=1**：`IMao-WinUI.csproj(161,29)` 报 `MSB4186`（`[System.IO.Path]::GetDirectoryName()` 静态方法调用无效），出现在**后段收集语言 MUI 文件**的步骤，与本次改动无关（是既有问题，报错点在属性/项求值，不在 XAML）。**但 app 与 `resources.pri` 在该步骤之前就已产出**，所以：
- 已用**字节扫描**验证新 `resources.pri` 确实包含新文案（含正/负对照，见 §26.1.2）；
- 只部署了 **`resources.pri`** 这一个文件（`68E9F694`），**没有**动 `IMao-WinUI.exe` / `IMao-WinUI.dll` / `IMao-WinUI.Core.dll`——它们与发布版字节不同只是因为提交号盖章（源码相同），没有理由替换已发布的二进制；
- 旧文件备份：`C:\Dapps\IMao\resources.before-mini-switch-20260921.pri`（`C144F8F7`）。

### 26.1.2 怎么证明新 XAML 真的进去了

`resources.pri` 是二进制，用 UTF-16 字节找字符串即可（本会话就是这么验的）：

| 探针 | 新构建 `resources.pri` | 发布版 `resources.pri` |
|---|---|---|
| `128=改用小地图局部覆盖层` | ✅ 命中 | ❌ 不含 |
| `64=改用 DirectComposition`（改动前就有） | ✅ 命中 | ✅ 命中 |
| 负对照（臆造的字符串） | ❌ 不命中 | ❌ 不命中 |


## 26.2 它到底改了什么

只改三样，都是任务书 §7 指定的：

```text
HWND 物理尺寸   = 小地图矩形 + padding（并裁剪在游戏客户区内）
SwapChain/backbuffer = 该 HWND 的客户区尺寸（复用 OverlayBackBufferSize::Ensure）
视口            = 同一块绘制里的这一小块区域
```

- **矩形算法**：小地图矩形取自 `GameWindowsScreenData::MinMapScreenData` 按客户区缩放；
  `padding = clamp(客户区宽 × 0.025, 30, 60)` px，四周相同，且不越出客户区。
- **2560×1440 下的实际数值**（按公式手算，**未实机核对**）：小地图 x 48→294.4、y 36.8→283.2；
  padding 60 ⟹ 左/上被裁剪到 0 ⟹ **355 × 344**。面积是 2560×1440 的 **3.3%**。
- **坐标系不变**：绘制空间仍是游戏客户区，所有 marker/路线/提示的坐标含义与今天完全一致；
  只有落在小地图矩形之外的部分不再显示。
- **窗口跟随**：复用 `OverlayWindowBounds::Synchronize` —— 矩形没变就**不调** `SetWindowPos`，
  并且按**实际几何回读**判定成功，所以没有引入新的高频 `SetWindowPos`。游戏窗口移动时矩形随之改变，照常跟随。
- **鼠标穿透**：窗口样式一个字节都没改（仍是 `WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | ...`）。
- **DPI**：mini 矩形与现有窗口同步走同一套物理像素（`ClientToScreen`），没有新增 DPI 分支；
  但 100%/125%/150% 三档**未实测**。

### 为什么用"平移顶点"而不是 `DisplayPos`

本仓库的 ImGui 把主 viewport 的位置**硬编码为原点**（`IMao-Core/src/Base/imgui_dx11/imgui.cpp` 中
`main_viewport->Pos = ImVec2(0.0f, 0.0f);`），于是每个 draw list 的裁剪矩形都从 `(0,0)` 起算；
而 DX11 后端会从裁剪矩形里**减去** `draw_data->DisplayPos`。两者在非零偏移下不自洽：
小地图在左上角时偏移恰好被裁剪成 `(0,0)`，看起来"能用"，换个分辨率/布局就会**裁错**。
因此改成把顶点整体平移到小窗口里：viewport 契约（`DisplayPos == viewport->Pos`）保持不变，
裁剪矩形依旧正确，ImGui 每帧都会重置顶点缓冲，平移不会累积。

## 26.3 已知的、固有的可见后果（不是回归）

实验模式下**小地图矩形之外的一切都不可见**。最直接的一条是**游戏内状态栏**：它画在客户区顶部居中
（`RuntimeStatusBar`），而小地图在左上角，所以实验模式下看不到"正在恢复定位 / 请打开一次大地图"这类提示。
marker 本身、数字角标、当前目标提示都在 padding 内，不受影响。超出 padding 的长路线段也会被裁掉。

这是"只维护小地图大小窗口"的必然结果。**正式落地时若要保留状态栏，需要它自己的小块窗口或把它并入小地图附近——那属于实验结论之后的设计问题。**

## 26.4 新增的日志字段（任务书 §13）

`overlay-motion` 行新增：

```text
overlayMode=full|mini
overlayWidth= / overlayHeight=      窗口客户区尺寸（= backbuffer 尺寸时即已匹配）
backBufferWidth= / backBufferHeight=
```

## 26.5 A/B/A/B 怎么跑

```powershell
# 管理员 PowerShell（PresentMon 需要独占 ETW 会话）
.\scripts\Measure-WorkIsolation.ps1 -Experiment mini-overlay
```

- 相位序列：`full-overlay-1(0) → mini-overlay-1(128) → full-overlay-2(0) → mini-overlay-2(128)`，即任务书要的 A/B/A/B。
- 每段默认 **45s**，段前有 **4s 沉降 + 5s 预热**（都不计入统计），脚本会提示你去诊断页改掩码。
- 输出：`out\perf\work-isolation.stats.txt`（FPS / p50 / p95 / p99 / max / >20ms / >33ms / **PresentMode**）与
  `.phases.txt`（每段起止时间戳，供复算）。
- 脚本会打印"两段参考相位（full）之间漂移多少"，**漂移 > 3 fps 就判定整张表不安全**——这正是任务书 §12 要的"同一次运行内交替"的护栏。
- 运行时要求：游戏中、前台、**同一个位置/视角/场景**、不战斗不开菜单，`开始探索` 已按、**大地图关闭**（否则 B 相位不会生效）。

## 26.6 状态

| 项 | 结果 |
|---|---|
| 编译 | ✅ 0 error（`ImGuiOverWindows.cpp`、`IsolationSwitches.h` 重编，`IMao-CoreHost` 链接成功） |
| 原生测试 | ✅ `IMaoOptimizationTests` / `IMaoMarkerTests` 通过 |
| 部署（原生） | `C:\Dapps\IMao\IMao-CoreHost.exe` = `7EABBC5B`（21:17），旧版备份 `IMao-CoreHost.before-perf-phase12-20260921.exe`（`45C99B91`） |
| 部署（托管） | `C:\Dapps\IMao\resources.pri` = `68E9F694`（含新开关文案与 255 上限），备份 `resources.before-mini-switch-20260921.pri`（`C144F8F7`） |
| 托管 shell「干净重建」 | ❌ **本环境做不到**：`IMao-WinUI.csproj(161,29)` 的 `MSB4186` 是既有问题；app/pri 在它之前已产出，故仅部署 `resources.pri`（见 §26.1.1） |
| 窗口几何 / DPI / 穿透 / 跟随 / Alt-Tab | **未验证**（需要实机会话） |
| 128 是否真的能填进诊断页 | **未验证**（需要你重启 app 后看一眼；`resources.pri` 的字节证据见 §26.1.2） |
| A/B/A/B 的 FPS / p95 / p99 / >20ms | **未验证**（这就是下一步要跑的东西） |

### 回滚（托管侧）

```powershell
Copy-Item 'C:\Dapps\IMao\resources.before-mini-switch-20260921.pri' 'C:\Dapps\IMao\resources.pri' -Force
```

---

# 28. Phase 4 实机 A/B/A/B 结果（2026-09-21 21:41:09–21:46:58）

命令：`.\scripts\Measure-WorkIsolation.ps1 -Experiment mini-overlay`（管理员 PowerShell，PresentMon 2.5.1，4 段 × 45 s，段前 4 s 沉降 + 5 s 预热不计入）。
相位：`full(0) → mini(128) → full(0) → mini(128)`。

## 28.1 PresentMon（同一场景、同一次运行内交替）

| 相位 | Frames | **FPS** | p50 | **p95** | **p99** | max | **>20ms** | >33ms | PresentMode |
|---|---|---|---|---|---|---|---|---|---|
| full-overlay-1 (mask 0) | 4646 | 102.1 | 9.58 | 12.49 | 14.50 | 41.81 | 0.09% | 0.02% | Composed: Flip 100% |
| **mini-overlay-1** (128) | 5094 | **111.9** | 8.84 | **9.84** | **10.86** | 15.82 | **0** | 0 | Composed: Flip 100% |
| full-overlay-2 (mask 0) | 4705 | 103.4 | 9.50 | 12.04 | 14.33 | 22.24 | 0.02% | 0 | Composed: Flip 100% |
| **mini-overlay-2** (128) | 5075 | **112.8** | 8.76 | **9.75** | **10.65** | 19.91 | **0** | 0 | Composed: Flip 100% |

- 参考相位漂移：full **1.3 fps**、mini **0.9 fps**（脚本阈值 ≤3 fps ⟹ 趋势可信）。
- **ΔFPS = +9.8 / +9.4（均值 +9.6 fps，相对 ≈ +9.3%）**；帧时间 9.79 ms → 8.90 ms，即**每帧省约 0.89 ms**。
- p95 −2.4 ms（−20%）、p99 −3.7 ms（−26%）；`>20ms` 从 0.09%/0.02% 变为 **0 / 0**。

**判定：任务书 §14 的 A（≥5 fps 且趋势一致）、B（>20ms 降 ≥50%）、C（p95/p99 明显下降）三条同时成立 ⟹ 值得正式落地。**

## 28.2 工具侧归属：收益在 present / 合成，不在 marker 绘制

| 相位 | 模式 | `segPresentMs`（每次 Present 平均） | `presentMs` 最大 | presents | presentSkipped | `markerDrawMs` | markerDraws |
|---|---|---|---|---|---|---|---|
| full-1 | full | **1.185 ms** | **40 ms** | 1134 | 246 | 0.024 | 1380 |
| mini-1 | mini | **0.423 ms** | **2 ms** | 1218 | 162 | 0.023 | 1380 |
| full-2 | full | 0.866 ms | 32 ms | 1196 | 244 | 0.024 | 1396 |
| mini-2 | mini | 0.438 ms | 1 ms | 1224 | 156 | 0.024 | 1380 |

- **`Present()` 本身便宜了约 2.5 倍，且长尾从 40/32 ms 塌到 2/1 ms** —— 这就是那 +9.6 fps 的来源：叠加层 present 不再长时间阻塞在合成上。
- **marker 绘制成本两模式完全相同**（0.024 ms、同 1380 次绘制）⟹ 收益**不是**"少画了 marker"。
- **present 次数没有减少**（mini 反而略多：presentSkipped 162/156 vs 246/244）⟹ 收益也**不是**"少 present 了"，而是**每次合成都更便宜**。
- `segBuildMs` mini 略高（0.95/0.97 vs 0.86/0.93 ms，+0.05~0.1 ms），与顶点平移那一步的开销一致；相对于游戏每帧省下的 0.89 ms，这是实验模式自身的小成本。

## 28.3 128 确实生效了（余量证据）

- mini 相位 `overlayMode=mini` **23/23**，full 相位 `overlayMode=full` 23/24 —— 相位内没有串档。
- mini 相位 `overlayWidth/Height = 355×344`、`backBufferWidth/Height = 355×344` ⟹ **SwapChain 也真的变小了**，不只是挪窗口（正是任务书 §6 要求区分的那件事）。
- 355×344 与 §26.2 按公式手算的预测**完全一致**。

## 28.4 不能外推的地方（必须写清）

1. **机制不是"回到独立翻转"**：四个相位的 `PresentMode` 全是 `Composed: Flip 100%`。收益来自"合成一块小得多的画面"，不是恢复了 Hardware: Independent Flip。
2. **本次场景很轻**：基线就有 **102 fps**、`>20ms` 仅 0.09%；而 `Docs/GameFrameCostAnalysis_20260918.md` 记录的现场是 **26 fps、>20ms 10.18%**。所以 **+9.6 fps 是本场景的数字，不能直接外推**到"掉帧 10–20 fps"的重场景；可迁移的是**相对改善（≈ +9.3%、p95/p99 −20%/−26%）**。要在重场景确认，需要同场景再测一次。
3. **状态栏在 mini 模式下不可见是固有的**，但它**不是**收益来源：marker 绘制耗时两模式相同，`segBuildMs` 也没降。
4. 仍未验证：DPI 100/125/150、鼠标穿透、D3D 设备重建、云同步刷新、长时间运行、以及"开大地图切回整屏"的实机行为。

## 28.5 结论与下一步

- **小尺寸 MiniMap Overlay 有明确、可重复的收益**（同场 A/B/A/B，两个 mini 相位都优于两个 full 相位）。
- 下一步（任务书 §14/§18）：**正式设计 Full Overlay / Mini Overlay 的切换**，并把"状态栏去哪儿"作为设计决策一并解决；大地图路径本轮不动。
- 在正式落地前建议补一次**重场景**复测，确认相对收益在基线 26–60 fps 的场景里仍然成立。

---

# 29. 正式落地第一步：小窗口成为默认 + 状态球（2026-09-21 22:0x）

设计与决策见 **`Docs/MiniMapOverlayDesign_zh-Hans.md`**（D1 已由玩家拍板：默认把状态栏缩成小地图右下角的**状态球**，只用颜色表示状态；完整状态栏作为一种可选模式，窗口扩成并集矩形）。

## 29.1 本次提交

| commit | 内容 |
|---|---|
| `52a6fc1` | 原生：mini 窗口成为**默认**（不再由开关开启）；bit 7 改为"**完整状态栏模式**"（并集窗口）；新增 bit 8 `kForceFullOverlay`（**强制整屏**，用于对照与回滚）；`RuntimeStatusBar::DrawCompact()` 画状态球、`ReservedBounds()` 供并集几何使用 |
| `c3d0fc2` | 诊断页掩码上限 255 → **511**，文案补上 128/256 的新含义 |
| `c6d94d1` | `Measure-WorkIsolation.ps1`：`-Experiment mini-overlay` 改为"极简 vs 完整栏"（都是小窗口），新增 `-Experiment full-client`（0 vs 256，重场景复测用） |

## 29.2 开关语义（诊断页那个框）

| 掩码 | 含义 |
|---|---|
| `0`（默认） | mini 窗口（2560×1440 → 355×344）+ 状态球 |
| `128` | 完整状态栏模式：窗口 = mini ∪ 状态栏矩形（→ 约 1505×344） |
| `256` | 强制整屏覆盖层（回到"今天"的窗口尺寸） |

## 29.3 状态球

- 位置：小地图矩形右下角内缩一个半径（2560×1440 下圆心约 (280,269)），**落在原有 padding 内，几何未变**。
- 半径：`max(8, 客户区宽 × 0.011 / 2)`。
- 颜色：**复用 `RuntimeStatusBar` 已有的状态色**，不新增状态判定；深色底盘 + 描边保证在亮地形上可读。
- 跟随 `statusBarEnabled`：玩家关掉游戏内状态栏时，球也不画。

## 29.4 验证状态

| 项 | 结果 |
|---|---|
| 编译 | ✅ 0 error（`ImGuiOverWindows.cpp`、`RuntimeStatusBar.cpp` 重编，`IMao-CoreHost` 链接成功） |
| 原生测试 | ✅ `IMaoOptimizationTests` / `IMaoMarkerTests` 通过 |
| 托管页面 | `resources.pri` 字节验证命中 `256=强制整屏覆盖层` 与 `128=状态栏改用完整样式`，负对照不命中 |
| 部署 | `C:\Dapps\IMao\IMao-CoreHost.exe` = `2D5DA023`（22:06）、`resources.pri` = `4D7282AB`（22:10）；备份 `IMao-CoreHost.before-perf-phase12-20260921.exe`（`45C99B91`）、`resources.before-status-ball-20260921.pri`（`68E9F694`） |
| **状态球的位置/颜色/可读性** | **未验证**（要实机看） |
| **默认翻转后的回归（§16 重点项）** | **未验证** |
| 并集窗口的收益（0 vs 128） | **未验证** |
| 重场景复测（0 vs 256） | **未验证** |
| DPI 100/125/150 | **未验证** |

## 29.5 D1b：已决策（2026-09-21）——**不做**

极简模式下只有颜色，**看不到文字提示**（例如冷启动时"请打开一次大地图以确定所在区域"）。玩家决定**保持纯颜色**，不为 hint 临时扩窗口或加文字行。
后果（已记录）：该提示只在**主窗口**或手动切到**完整状态栏模式（掩码 `128`）**时可见；球用颜色区分恢复/暂留/故障。若以后觉得不够，D1b 的两个备选（mini 窗口内画一行小字 / hint 时临时切完整栏）在设计书 §5 D1 里留着。

## 29.6 实机反馈修掉的一个真 bug：窗口在每次状态切换时弹回整屏（2026-09-21 22:1x）
**玩家报告**：刚启动开始探索时中间的状态栏会出现，定位成功后切成小球，"貌似有点问题"。

**日志证据**（`events-20260921.jsonl`，会话 22:15:56–22:18:10）：

```text
22:16:17  mode=full  2560x1440   ← 启动、小地图尚未被检测到
22:16:23  mode=mini  355x344     ← 检测到小地图
22:16:49  mode=full  2560x1440   ← Gameplay->Unknown
22:16:51  mode=mini  355x344     ← Unknown->Gameplay
22:17:12  mode=full  2560x1440   ← 又一次 Unknown
```

**根因**：我把小窗口的条件写成 `frame.minimapVisible`，而它要求 `isExistMinMap`（小地图被**正面检测到**）。于是每个 `Unknown` 帧（启动阶段、每次过图/菜单过渡）都退化为**整屏窗口 + 居中状态栏**，随后再跳回小窗口。55 秒内跳 3 次；而且 `Unknown` 期间恰恰付出了这次改动要消除的**整屏合成代价**。设计书 §3 写的本来就是"定位不可信 / 小地图缺失 → 保持 mini 尺寸"，是**实现与设计不一致**。

**同时发现的第二个坑**：状态球原本取 `frame.minimapMarkers.center/radius`，而这两个值只在**定位成功过**之后才有；冷启动阶段为 0 ⟹ 只修窗口条件的话，冷启动会**连球都没有**（一片安静）。

**修复**（`2bccb8a`，构建 `9D95B21B`）：
1. 窗口条件改为**"大地图未打开就是小窗口"**（只有大地图需要整屏，因为视口 marker 可能落在任何地方）。
2. 球改为按 **HUD 名义小地图矩形**（与窗口尺寸同一个来源）定位，不再依赖 frame 里"上次已知的圆"⟹ 第一帧就正确。

**验证**：编译 0 error、两套原生测试通过；**"跳变是否真的消失"未实机确认**。

## 29.7 第二轮实机反馈：球的门控 + 极简不再画中间栏 + 球开关（2026-09-21 22:4x）

**玩家报告三件事**：
1. 从小地图切到大地图、在大地图被识别出来之前，小地图的**状态球会保留一段时间**；识别不到小地图时就不该再画。
2. **极简模式下大地图仍显示中间那条状态栏**；极简模式应当只保留小地图的状态球。
3. 给极简模式加一个**复选框：是否显示状态球**，勾上显示、不勾不显示，**默认不勾**。

**原因**：上一轮修窗口条件时，我把球的条件写成了"窗口是小的就画球"，而窗口条件已经放宽为"大地图未确认就小窗口" ⟹ 过渡期与大地图未识别期都在画球；而 `else` 分支仍无条件画完整状态栏。

**修复**（`6dcca0e` / `f340c87`，构建 `89DD1585` + 托管 `2F1B74A2`/`7D78AB63`）：

| 规则 | 现在的条件 |
|---|---|
| 画球 | 极简样式 **且** `frame.minimapVisible`（小地图是被检测到的当前状态）**且** 新设置 `statusBallEnabled`（默认 **关**） |
| 画完整状态栏 | **只有**完整状态栏样式（掩码 `128`）才画；极简样式**永不**画它（含大地图） |
| 球的显示还需 | `statusBarEnabled` 为真——关掉"游戏内状态条"表示不想要游戏内状态显示，此时球也不画 |

**新设置**：设置页「地图显示」区新增 **「小地图状态球」** 开关（与「游戏内状态条」并列），随 `runtimeConfiguration` 持久化并下发（`statusBallEnabled`）。**默认不勾** ⟹ 极简模式下默认只有 marker，没有任何状态显示。

**部署**：`C:\Dapps\IMao\` 的 `IMao-CoreHost.exe` = `89DD1585`、`IMao-WinUI.dll` = `2F1B74A2`、`resources.pri` = `7D78AB63`；
备份 `IMao-WinUI.before-status-ball-toggle-20260921.dll`（`6BEA17F6`）、`resources.before-ball-toggle-20260921.pri`（`4D7282AB`）、以及原有的 `IMao-CoreHost.before-perf-phase12-20260921.exe`（`45C99B91`）。
字节验证：新 `IMao-WinUI.dll` 含 `statusBallEnabled`（旧的不含）、新 `resources.pri` 含 `小地图状态球`，负对照不命中。

**验证状态**：编译 0 error、两套原生测试通过、产物字节验证通过；**球的新门控与开关的实机表现未验证**。

## 29.8 第三轮实机反馈：两个开关必须互相独立（2026-09-21 22:5x）

**玩家报告**：设置页里「游戏内状态条」和「小地图状态球」都开着，但**状态栏根本不显示**；并明确要求：两个开关**独立互不干扰**——状态条开关管状态条，球开关管球。

**原因**：我在 §29.7 把模型做成了"极简样式永不画状态栏"（为了让极简模式在大地图上也不出现中间那条栏）。这是我把"样式"当成了一个整体模式，而玩家要的是**两个独立元素各自一个开关**。

**修复**（`6dcca0e` 之后的这一轮）：

| 项 | 现在的行为 |
|---|---|
| 状态栏 | `statusBarEnabled` 打开就画（`RuntimeStatusBar::Draw` 自己的门控：核心运行中 + 游戏窗口是显示上下文），**与球无关** |
| 状态球 | `statusBallEnabled` 打开且在**小地图是被检测到的当前状态**时画，**与状态栏无关** |
| 窗口尺寸 | 小地图矩形 + padding；**状态栏开着时并入状态栏矩形**（否则窗口装不下它 ⟹ 就会重演"开关打开却没有栏"） |
| `RuntimeStatusBar` 内部 | 新增 `layout.available`（状态模型可用）与 `layout.visible`（= available && statusBarEnabled）分离，球只看 `available`，栏才看 `visible` |
| 隔离掩码 | **bit 7（128）删除**（不再有"样式"可选）；诊断页文案改为"128 已废弃"；`256` 保留为强制整屏 |

**测量脚本**：`-Experiment mini-overlay` → 改为 **`-Experiment status-bar`**（A/B/A/B；相位提示改为"在设置页把「游戏内状态条」关/开"，掩码全程 0），因为状态栏已是设置项而非掩码位。

**部署**：`IMao-CoreHost.exe` = `3089D229`（22:54）、`resources.pri` = `DCB2D4E0`（22:56，含"128 已废弃"）；`IMao-WinUI.dll` 沿用 `2F1B74A2`（本轮只改 XAML 文案，代码未变）。备份：`resources.before-128-retired-20260921.pri`。

**验证状态**：编译 0 error、两套原生测试通过、pri 字节验证通过（含"128 已废弃"、仍含"256=强制整屏覆盖层"与"小地图状态球"）；**两个开关的实机行为未验证**。

## 29.9 第四轮：状态条按"大地图内 / 大地图外"再拆开（2026-09-21 23:1x）

**玩家要求**：把"大地图界面内的状态条"和"大地图之外的状态条"分成两个设置，自由组合。

**实现**：状态显示从两个开关变成**三个独立开关**。

| 设置项 | 字段 | 默认 | 生效范围 |
|---|---|---|---|
| 状态条 · 小地图 | `statusBarEnabled` | 开 | `gameState != "bigMap"` 的每一帧（含启动、过图过渡） |
| 状态条 · 大地图 | `mapStatusBarEnabled`（**新增**） | 开 | `gameState == "bigMap"` |
| 小地图状态球 | `statusBallEnabled` | 关 | 仅小地图是被检测到的当前状态时 |

- `RuntimeStatusBar::Prepare` 用 `RuntimeStatus::Snapshot().gameState` 决定这一帧的栏归哪个开关管；`layout.available` / `layout.visible` 的拆分保留（球只看 available，栏才看 visible）。
- 窗口并集仍然只看「状态条 · 小地图」（大地图状态本来就是整屏窗口）。
- 设置页把原来的「游戏内状态条」改名为 **「状态条 · 小地图」**，新增 **「状态条 · 大地图」**（持久化键沿用/新增，不改用户数据格式版本）。
- ⚠️ **迁移**：`mapStatusBarEnabled` 默认开 ⟹ 原来把状态条整个关掉的玩家，大地图里会重新出现状态栏，需要一并关掉。已写进设计书。

**部署与验证**：

| 文件 | 版本 | 备份 |
|---|---|---|
| `IMao-CoreHost.exe` | `A0BC8836`（23:15） | `IMao-CoreHost.before-perf-phase12-20260921.exe` |
| `IMao-WinUI.dll` | `6D8CE989`（23:16） | `IMao-WinUI.before-split-bars-20260921.dll` |
| `resources.pri` | `C31B36B1`（23:16） | `resources.before-split-bars-20260921.pri` |

编译 0 error、两套原生测试通过；字节验证：新 dll 含 `mapStatusBarEnabled`（旧的不含）、新 pri 含 `状态条 · 大地图` 与 `小地图状态球`，负对照不命中。
**未验证**：三个开关的实机组合行为、并集窗口收益、DPI 三档。


**副作用（要留意）**：修好之后，**冷启动阶段不再有中间那条文字状态栏，只有一个球**。这是 D1b（保持纯颜色）的直接后果——文字提示只在主窗口或掩码 `128` 的完整状态栏模式下可见。



---

# 27. 遗留：§25.1 的判据原本要靠推理

§25.1 的判据靠"候选集 id 仍在 + 绘制数 −1"来推理，因为候选集与绘制数之间还夹着**迟滞环**（候选用 `minMapRadius+16/+48`，绘制用严格 `minMapRadius`）——单独一个点也可以因落在迟滞环里而"在候选、不被画"。
要让它变成单因素证据，只需在同一个累加器里加一个计数：**本帧因 `isSaved` 而被跳过的候选数**（`markerSkippedCompleted`）。已由 `b4f70e4` 落地（`overlay-motion` 行新增 `markerSkippedCompleted=`），届时日志会直接写出"完成了 1 个点 → 跳过计数 = 1"；Phase 4 那一次实机测试可以顺带把它验掉。



