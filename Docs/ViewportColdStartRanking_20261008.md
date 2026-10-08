# 大地图冷启动：先用视觉索引排序，再决定搜什么

日期：2026-10-08
状态：已实现，本机测试通过，待实机验证
相关：`Docs/VisualIndexRebuildRegression_20261008.md` §9 第 4 项

## 一、起因：一个实机观察

玩家观察到，**冷启动时停在小地图界面，匹配速度往往比停在大地图界面更快**。

这个观察成立，而且不是错觉，原因是两条管线在"没有先验"这件事上的含义完全不同。

|  | 小地图界面 | 大地图界面 |
|---|---|---|
| 管线 | `GlobalVisualLocalizer` | `MapViewportLocalizer` |
| 查询图像 | `hud::kMinimap` = 184×177 | `hud::kMapCenterArea` = 1440×765（约 65 倍面积） |
| 尺度 | 已知（`Scene::MinimapScale`，默认 194/184） | 未知，玩家可缩放 |
| 候选怎么来 | 视觉索引倒排表排序，上限 `kMaximumCoarseCandidates = 12` | 对该场景**全部**瓦片建 FLANN，暴力 `knnMatch` |
| 冷启动时 | 只少一个提示，算法路径不变 | scope 落到 `Global`，即最贵的那一档 |
| 实测单次 | 中位 **4.8 ms** | 中位 **128 ms**、p90 352 ms、max 10.7 s |

数字来自 `measurements/accuracy-check/viewport_latency.py` 对既有日志的统计（记录在
`Docs/VisualIndexRebuildRegression_20261008.md` §10）。注意大地图那个中位数基本就是
`local-512` 的成本——571 次提交里 519 次是它，而且是接受率 91% 的正常路径。`Global`
远在这个之上，接受率只有 **13%**。

### 为什么冷启动把大地图顶到最贵的档位

`App.cpp` 提交时的 scope 选择：进入 `Local512` 需要 `prediction.confidence >= 2` 且
`sceneId` 已准入，或者 `activeWorldSearchPrior` 有效（要求 `playerLocationLock.valid`）。
冷启动两个都不成立，日志里就是那句
`scene-search-prior: used=false reason=no-confirmed-player-location`。

而 `prediction.sceneId` 本身只能来自一次绝对解算——**鸡生蛋**。

于是 `MapViewportLocalizer::Locate` 在 `Global` 下的代价是：

```
2 个裁剪区域（中央 350×350 + 整幅）
× 4 个缩放因子 { 1.0, 1.5, 2.0, 0.75 }   ← 非 Global 时只试 1.0
× 最多 9 个已准入场景
= 最坏 72 次 knnMatch，每次对手是该场景的全部瓦片
```

每次 `GetLocalMatcher` 首次都要复制 6–19 MB 候选并训练一个 FLANN 索引。所以冷启动的主要
成本不是 `knnMatch` 本身（0.1–0.4 ms），而是**为 9 个场景各建一次大匹配器**，其中 8 个注定
不是答案。

## 二、设计

一句话：**让大地图借小地图那套排序来选场景，但排序只能"缩小范围"，不能"下结论"。**

1. **检索预扫**（仅冷启动）。取这次请求本来就要用的"地图中央裁剪、原始尺度"图像，抽一次
   SURF 描述子，用视觉索引倒排表打分——和小地图的粗检索是同一段代码
   （`VisualIndexRetrieval::ScoreTiles`）。代价是几毫秒。
2. **按场景聚合**。每个场景取它自己得分最高的那块瓦片作为该场景分数（与小地图在单个网格
   单元内的 `max` 策略一致：同一地区的副本是重复观测，不是独立证据）。
3. **决定缩小到什么程度**：
   - 冠军领先亚军达到 `kRetrievalSceneLead = 1.5` 倍 → 只搜冠军这一个场景，并且用它得分
     最高的 `kRetrievalHintTiles = 12` 块瓦片算出一个中心与半径，把搜索限制在这个邻域内
     （等价于一次 `Local512`）。
   - 两个场景咬得很近 → **两个都留**，不做瓦片限制。两个坐标平面看起来一样，正是调用方已有
     的 `acceptedScenes > 1` 歧义判定要处理的情况，让那个判定看见它们。
   - 排序为空 / 索引建不起来 / 邻域一块瓦片都选不出来 → 完全按原样，走全扫。
4. **兜底全扫**。每个冷启动请求会构造**两个** plan：先是排序选出来的那个，然后是这个路径一直
   在做的全扫。**没有第二个 plan，一次排错就会丢掉一次本来能成功的定位**，所以全扫是地板，
   排序只能比它更好，不能取代它。

其余请求（带先验的 `local-512` / `local-1024`）只构造一个 plan，就是请求本身——**它们的路径
一个字节都没变**，也不为这个排序付出任何代价。

### 为什么中心取 top-12 的均值、半径还要覆盖它们

预热时的 `Local512` 先验半径是 512，接受率 91%，所以"512 半径的瓦片集"是被证明可用的匹配器
规模。区别只在中心从哪来：预热时是玩家坐标，这里是排序喜欢的那些瓦片。

一块瓦片是 384 单位，而大地图视口远不止一块瓦片宽，所以**得分第一的那块只是"很多正确答案
之一"，不是答案**。因此中心取 top-12 的中心点均值，半径从 512 起、扩大到能覆盖这 12 块
（按搜索自己用的矩形距离口径算，再放宽一块瓦片），上限 2048。

### 为什么阈值是猜的，以及怎么不靠猜

`kRetrievalSceneLead = 1.5` 是一个**保守的估计**，不是量出来的。所以它被刻意放在"宁可多搜"
的一侧：领先不到 1.5 倍就走两场景路径，仍然把 9 个场景砍到 2 个。

实机日志会直接给出校准它需要的数据：每条 `map-viewport-result` 同时带
`scene=<最终接受的场景>` 和 `retrievalScene=<排序点名的场景>`，两下一比就是排序准确率。
字段清单见第四节的表。

## 三、失败模式

| 情况 | 行为 |
|---|---|
| 排序点名了错误场景 | 该 plan 失败 → 同一次调用里继续跑全扫。多花一个小匹配器，不丢定位 |
| 排序把真实场景排到第 3 名及以后 | 同上，全扫兜住 |
| 提示邻域没盖住真实位置 | 同上 |
| 索引载不进来 / 词表形状不对 | `BuildVocabularyIndex` 返回空 → 不做预扫，行为与改前完全一致 |
| 查询描述子一个视觉词都没命中 | `ScoreTiles` 返回 false → 同上 |
| 索引有词表但没有 posting | 排序成功但全零分 → 视为"没排名"，同上 |

## 四、日志字段

`map-viewport-result`（接受与拒绝两条都有）新增：

| 字段 | 含义 |
|---|---|
| `retrievalRanked` | 打分不为零的场景数。0 = 排序没点名，走了全扫 |
| `retrievalScenes` | 排序保留的场景数（1 或 2） |
| `retrievalScene` | 排序点名第一的场景 |
| `retrievalTop` / `retrievalRunnerUp` | 冠军 / 亚军分数，用来校准 `kRetrievalSceneLead` |
| `retrievalTiles` | 瓦片提示留下的瓦片数。0 = 只缩了场景，没有瓦片提示 |
| `retrievalMs` | 排序本身耗时 |

新增字段都追加在行尾，且名字里不含 `scope=` 或 `durationMs=`，所以
`measurements/accuracy-check/viewport_latency.py` 的既有正则**无需修改**。

## 五、改了哪些文件

| 文件 | 改动 |
|---|---|
| `IMao-Core/src/Feature/VisualIndex/VisualIndexRetrieval.h` | **新增**。共享的打分：描述子 → 视觉词 → TF-IDF → 每块瓦片一个分数。两个定位器共用，加权方式不会各自漂移 |
| `IMao-Core/src/Coordinate/VisualLocalization/GlobalVisualLocalizer.cpp` | `RetrieveTiles` 的算分部分改调共享模块；分组与提示过滤（本路径自己的策略）原样保留。**行为不变** |
| `IMao-Core/src/App/MapViewportLocalizer.h` | 结果结构体新增 7 个排序诊断字段 + `MapViewportRetrievalFields()` |
| `IMao-Core/src/App/MapViewportLocalizer.cpp` | 新增 `VocabularyIndex()` / `RankScenes()` / `BuildPlans()`；`Locate` 的 region/factor/scene 三层循环外面套一层 plan 循环 |
| `IMao-Core/src/App/App.cpp` | 两条 `map-viewport-result` 追加排序字段 |
| `IMao-Core/tests/OptimizationTests.cpp` | 新增 `TestVisualIndexRetrieval()` |

`MapViewportLocalizer::Locate` 里还有一处顺带的节省：地图中央裁剪的 SURF 描述子现在只抽一次，
排序和第一个裁剪区域共用。**只有冷启动会提前抽**——`local-512` 是常见路径，不能为一次它用不上
的排序付钱。

## 六、验证

| 检查 | 结果 |
|---|---|
| `IMaoOptimizationTests` | 通过。新增的 `TestVisualIndexRetrieval` 覆盖：命中词表行的描述子给对应瓦片打分、相反描述子给另一块、没有 posting 时"排了但全零"与"拒绝查询"可区分、空查询与错误宽度被拒、词表形状不符不建索引、偏移量溢出时以载荷为界且越界 tileIndex 被跳过 |
| `IMaoVisualRegression --scene-self-test` | 39 项全 PASS。这一项同时编入两个定位器与 `MultiSceneViewportTests`，覆盖了"索引排不了名时按原样全扫"这条兜底 |
| 全量目标编译 | 见本次提交记录 |

## 七、实机要看的

1. 冷启动开大地图，**首次识别耗时**是否下降（对比 `Docs/VisualIndexRebuildRegression_20261008.md` §10 的
   基线：4/22 个会话首次识别超 5 秒，最长 32 秒）。
2. `retrievalScene` 与最终 `scene=` 是否一致——这就是排序准确率。
3. `retrievalRanked=0` 的比例。偏高说明大地图视口的描述子对不上索引，排序这条路本身不成立。
4. `retrievalTiles` 的分布。若普遍偏小（几十块），要确认不是提示太紧；若普遍顶到 2048 上限，
   说明 top-12 散得太开，均值中心这个做法要重新考虑。
5. 排序点名错误场景时，多出来的那次小匹配器有没有让**最坏情况**明显变差。

## 八、不做什么

- **不动 `local-512` / `local-1024` 的路径**。那是 91% 接受的正常路径，本次只碰冷启动。
- **不让排序直接决定结果**。它永远只是一个 plan，后面跟着全扫。这条是设计的核心约束，
  不是实现细节。
- **不动 `kRetrievalHintMaxRadius` 之外的内存预算**。`GetLocalMatcher` 的缓存上限（48 MB /
  8 项）本来就已经挡不住全扫的 9 个场景，本次没有改它；实机第 5 点如果显示 churn 变差，
  再单独讨论。
- **不解决"大地图切区域后识别慢（10–20 秒）"**。那是 `Docs/VisualIndexRebuildRegression_20261008.md`
  §9 第 4 项的另一个问题，已有它自己的那一处修复。本次改动顺带让它的 `Global` 升级也走排序，
  但没有针对它做设计。
