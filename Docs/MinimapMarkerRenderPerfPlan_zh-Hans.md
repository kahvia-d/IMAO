# IMAO 小地图标记渲染掉帧优化任务书

> **这份文件是本次性能优化工作的任务书 + 进度台账，不是完成后的一次性记录。**
>
> - 分支：`perf/minimap-marker-render`，从 `main` = `688e100`（`Say what a release note has to be before it is signed`）切出。
> - §1～§19 是**任务书原文**，逐字保留，避免走样或遗忘；§20 起是**开工前的现状核实**与**进度台账**。
> - 记录纪律：**没有实测的数据一律写「未验证」**。禁止写"预计已解决 / 应该已经优化 / 理论上不会掉帧"。
> - 每完成一个阶段就地更新 §21 台账、并把该阶段的状态行改成「已实现（待实测）」或「已验证」。
>
> 状态：**Phase 0 准备完成；Phase 1 / Phase 2 已实现（均未实测）；Phase 3～Phase 6 未开始。**（明细见 §21）

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

* `MarkerCompletionStore::Completed()` 取的是 `std::scoped_lock mutex`，而**写路径 `Commit()` 是在持锁状态下
  写文件**（`MarkerCompletionStore.cpp` 对应 `.h:436` `Commit`）。所以渲染线程现在这条查询不只是加锁，
  还可能**阻塞在磁盘写**上——这正是"帧时间长尾"的合理候选机制之一。Phase 1 会把它从渲染路径整体移走。
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

| 阶段 | 内容 | 状态 | commit | 实测数据 |
|---|---|---|---|---|
| Phase 0 | 开分支 + 任务书落档 | 已完成 | `docs: …`（见 git log） | 不适用 |
| Phase 1 | 渲染线程不再实时查询 completion | 已实现（**未实测**） | `perf: use marker snapshot completion state during minimap rendering` | 未验证 |
| Phase 2 | 纹理查找 O(1) | 已实现（**未实测**） | `perf: add constant-time marker texture lookup` | 未验证 |
| Phase 3 | marker 分段性能日志 | 未开始 | — | 未验证 |
| Phase 4 | 真正的小尺寸 MiniMap Overlay 实验模式 + A/B/A/B | 未开始 | — | 未验证 |
| Phase 5 | Texture Atlas | 未开始（**取决于 Phase 4 结论**） | — | 未验证 |
| Phase 6 | WGC ROI Readback | 未开始（**独立后续实验**） | — | 未验证 |

## 回归检查（任务书 §16，逐项待实测）

16 项全部**未验证**。Phase 1 相关的第 3、4、6 项优先，Phase 4 相关的第 9～15 项在实验模式落地后测。
