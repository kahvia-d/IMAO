# 分层楼层该解除时不解除：地表就在洞穴正上方（2026-10-01，roysurface 天槎空间站）

## 一、现象

用户在 `天槎空间站` 一带：**大地图 46 个标记，小地图只剩 2 个**，浮层一直显示"小地图追踪中"。
他给的层级顺序是"盲望之塌 → 天槎空间站 → 封锁舱段 → 实验舱段一区 → 实验舱段二区"，
并且怀疑"盲望之塌 这个层级有问题，它应该是最上层"。

## 二、先说数据：层级顺序没问题，`盲望之塌` 根本不是分层楼层

- `Assets/KuroMap/country.json`：`盲望之塌` 是 **level-2 节点、`mapState=6`** —— 它就是我们注册表里的
  **`roysurface` 这个地区本身（地表）**；`天槎空间站` 是它下面的 level-3 节点、`mapState` 为空。
- frame 8 的 30 个图层里**没有**叫 `盲望之塌` 的图层或楼层。图层 **44「天槎空间站」只有 4 层**：
  `-1/44` 天槎空间站、`-2/44` 封锁舱段、`-3/44` 实验舱段一区、`-4/44` 实验舱段二区。
- 我们发布的 `roysurface` 索引里这 4 层是 `heightRank=-1/-2/-3/-4`、`heightDirection=1`
  → **天槎空间站最高**，往下依次是封锁舱段、一区、二区。

所以用户描述的"自上而下"与数据一致（把地表 `盲望之塌` 放在最上面也是对的）：**不是层级排错，也不是数据缺失**。

## 三、真正的原因：采纳之后，只有"离开足迹"才能解除

`LayeredMapState.cpp` 的 `ObserveMinimap` 在分类**没有**命名任何楼层时（`else` 分支）这样决定是否保留：

```cpp
const bool stillInside = current.active && active != entries.end() &&
    LayeredFloors::Contains(active->floor, active->transform, mapX, mapY);
if (current.active && unknownCount >= kClearFrames && !stillInside) { /* 清除 */ }
```

而 `LayeredFloorIndex.h` 定义 `Contains` 时就写明了这个前提不成立：

> standing on the surface ABOVE a cave shares the coordinate, so containment can never decide
> "am I in a layer", only "which one".

天槎空间站的地图画在 `盲望之塌` 那块雪地**下面**，足迹必然重叠。日志（2026-10-01 那次会话）：

```
11:50:41  floor=-3/44 active=1 identified=1 ownShare=1.00    ← 玩家确实在实验舱段一区里
11:52:04  floor=-3/44 active=1 identified=0 ownShare=0.077   ← 已经走出来，影像不再指向它
11:52:09…11:56:32  floor=-3/44 active=1 identified=0 ownShare=0.000
          containing=[-1/44 -3/44 -4/44]                     ← 位置一直在足迹内，从未离开
```

后果由 `RoleFor` 决定：`SurfaceRole(state) = sharedGround || openToSurface ? Normal : Hidden`，
而这个粘住的层是**封闭**层（`open=0`）⟹ **所有地表标记、以及其他分层地图的标记全部 Hidden**，
只剩它自己（及等价层）的 2 个标记。大地图不做分层过滤，所以 46 个都在。

> 这与 `Docs/LayeredMapFalsePositive_Hukou_20260926.md` 是同一类问题的**另一半**：那次修的是
> "地表帧**误采纳**了洞穴层"（用 own-art 份额否决），这次是"采纳之后**该解除时不解除**"。

## 四、修法与判据（先量后定）

采纳端早就有正确的判据：**洞穴帧与地表帧的区别不在总数，而在"这些匹配来自该层自己画的art吗"**
（own/total：地表 0.00-0.14，洞内 0.24-0.90，见 `Classification::ownDominant`）。
保留端现在用同一把尺子，而不是足迹：

- `kRetainOwnShare = 0.20`：被保留的那一层，本帧自己的匹配数 / 它的总匹配数 ≥ 0.20 才算"还支持它"；
- 连续 `kClearFrames`（10）次分类都不支持，就清除——**即使位置仍在足迹内**；
- 支持就继续保留，无论识别多弱（`眠龙庭·上层` 在洞里只有 2-5 个 own 匹配，绝不能被误清）。

同一份日志的量化（`layered-floor` 行的 `ownVotes` 里取被保留层的 own 匹配数）：

| 状态 | 被保留层 own 匹配 | ownVotes 名次 |
|---|---|---|
| 洞内（11:50:41–11:51:02） | 6 / 9 / 11 / 6 / 9 / 15 | **第一名**，领先第二名 2~7 倍 |
| 地表（11:52:04 起） | 3 → 0，多数帧直接不在表里 | 并列或消失 |

日志也补了两个字段：清除时记 `reason=left-footprint | unsupported`，周期行新增
`activeOwn=` / `activeTotal=` / `activeShare=`，下一次会话能自己解释自己。

## 五、验证

- `IMao-Core/tests/LayeredMapTests.cpp` 新增一例，用真实序列驱动（第一次由测试直接驱动
  `ObserveMinimap`）：先 `SetForTest` 成"已在 -3/44 里"，喂 12 帧"识别不出来但仍全是自己的art"
  （5/5）**必须保留**，再喂 12 帧完全不相关的帧（地表）**必须清除**，并且全程断言
  `LayeredFloors::Contains(...) == true` —— 说明清除靠的是影像，不是足迹。
- **对照实验**：把新条件去掉（退回只认足迹）⟹ 正好那一条断言变红；恢复后全绿。
- **门禁漏洞一并补上**：`IMaoLayeredMapTests` 从 2026-09 起只注册在 ctest 里，而
  `scripts/Test-Runtime.ps1` **既不构建也不运行它**（它不跑 ctest）——所以这类规则可以悄悄腐烂。
  现已加入构建目标列表并与其他原生套件一起运行（`layered-map-tests.log`）。
- `scripts/Refresh-MapTestBinaries.ps1` 的新警告此前在**健康**的树上也会报（且 `{0}` 没被替换）。
  现在只在"这棵树加载了暂存登记表里没有的包"时才响——那才是下次暂存会把它冲掉的条件。

## 六、残留与边界

- 判据是**统计**的：10 次分类（约 3 秒）才清除，所以走出洞穴后标记会晚几秒回来；
  期间画面与之前一致（标记隐藏）。这是有意用时间换稳定：单帧翻转会让整套标记闪烁。
- `sharedGround`（复制地表地面的那一层）与 `openToSurface`（开放分层）仍各自独立生效；
  本次改动不碰它们。
- 仍可能出现的相反错误：在洞里连续 10 帧都读不出该层的 own art（例如极薄的特征区），
  会把层清掉 ⟹ 地表标记短暂回来。比"永远粘住"轻，且下一次决定性分类会重新采纳。
