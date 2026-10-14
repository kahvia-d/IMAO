# even 诊断包新日志：点位过滤阻塞与源帧过期（2026-10-14）

分支：`codex/even-map-marker-flicker`。程序源码保持诊断包 `2026.10.13.101` 的状态，
本轮分析不修改显示规则、不改既有测试断言、不重打包或发布。

## 已确认的因果链

这次日志把 10 月 13 日的判断闭合到具体环节：

**大世界的点位过滤 / 更新阶段需要约 250–290 ms → 标记源帧生产约 310–350 ms
一次 → 发布时已耗掉 500 ms 寿命的大半 → 下一个源帧产出前，旧源帧先过期 →
渲染撤掉整层 → 新帧回来再显示。**

下层金库的过滤约 7.6 ms，整轮约 40 ms；相同配置下能跟上 80 ms 更新节拍，
因而稳定窗口没有源帧过期拒绝。该差异与玩家“大世界闪、下层金库正常”的反馈一致。
可见性与配准仍各有独立门禁，但本次大世界的主要撤绘原因已经明确是源帧过期。

这不是“异步定位一直失败”的表现。热路径应优先修复
`DrawItemOnGameMap::UpdateCenterPointNearItemsData` 内部的过滤与完成状态读取成本。
**不能把 500 ms 门槛调大或刷新旧源帧时间当作已经修复性能问题。**

## 输入与测试版本核验

- `Logs (3).zip` SHA-256：
  `f0c79a07ca875f75b63c3037869b2f5da7030c4d3805d3dcbbc55a51aab47a6d`。
- 解包保存在本地 `out/evidence/even-map-marker-flicker-20261014/`，不进入 Git。
- 附件包含旧版和测试版的多次运行，不能全文件混合统计。
- `startup.log` 在 10 月 14 日 08:42:51（UTC+08）明确记录
  `snapshot=bundled-2026.10.13.101`；对应 CoreHost 会话从 08:43:18 开始。
- 该会话同时具有 `map-overlay-cycle` 以及 `mapStaleFrames` 等新增字段，
  证明新增诊断确实在玩家侧运行。
- 更早 02:29:48 的诊断会话使用 `updateMapMs=240`。下面主对照使用后一次会话，
  三个阶段的 `updateMapMs=80`、`updateMinimapMs=80`、源帧寿命 500 ms 均一致。

## 大世界 → 下层金库 → 大世界

按诊断会话与已接受视口的场景身份分段，并在切换两侧保留数秒余量，避免把跨场景的
两秒累计计数混在一起。生产阶段的数据按 `map-overlay-cycle.scene` 选择；
渲染阶段只选择 `mapRequestedFrames > 0` 的记录。

| 稳定窗口（UTC+08） | 生产样本数 | 过滤耗时中位数 | 整轮耗时中位数 | 发布时帧年龄中位数 | 过滤占整轮比例中位数 |
|---|---:|---:|---:|---:|---:|
| 08:43:30–08:45:04，大世界 scene=1 | 44 | 254.40 ms | 312.83 ms | 359.54 ms | 80.30% |
| 08:45:15–08:47:19，下层金库 scene=6 | 57 | 7.61 ms | 40.08 ms | 77.88 ms | 20.45% |
| 08:47:35–08:49:14，返回大世界 scene=1 | 46 | 292.28 ms | 352.66 ms | 387.23 ms | 82.03% |

同一窗口的渲染门禁统计：

| 场景 | 请求绘制迭代 | 成功附着 | 源帧过期 | 上下文 / 尺寸拒绝 | 可见性拒绝 | 图像附着失败 |
|---|---:|---:|---:|---:|---:|---:|
| 第一次大世界 | 2266 | 791 | 1458 | 0 | 17 | 0 |
| 下层金库 | 3481 | 3297 | 0 | 23 | 161 | 0 |
| 返回大世界 | 2536 | 950 | 1586 | 0 | 0 | 0 |

**返回大世界的所有拒绝都是源帧过期**，占请求迭代的约 62.5%。
这排除了焦点、可见性证据、图像附着失败作为该窗口主要撤绘原因。
下层金库依然有少量上下文 / 可见性拒绝，不能说它永远没有其他撤绘。

整个 08:43 会话中，scene=1 的 104 条渲染采样累计：请求 5152、附着 1804、
源帧过期 3317、上下文拒绝 1、可见性拒绝 30、附着失败 0。
因此源帧过期占该会话大世界**全部拒绝的约 99.1%**。

## 可定位到日志原行的例子

`events-20261014.jsonl:12628`，08:47:38：

```text
scene=1 predictMs=26.3113 resultMs=38.1058 searchMs=1.9032
filterMs=284.3094 publishMs=1.0190 cycleMs=351.6523
captureAgeAtStartMs=48.5003 publishedAgeMs=400.1526 maximumAgeMs=500
updateMapMs=80 updateMinimapMs=80 mapVisible=1 reliable=1 markers=252
```

发布时还剩约 100 ms 寿命，却需要约 350 ms 完成下一轮，途中必然出现过期空档。
帧仍然可靠且具有 252 个候选标记；不是“没有可画的点”。

`events-20261014.jsonl:9497`，08:44:19：

```text
sourceAgeMs=555 sourceMaximumAgeMs=500 visibilityAgeMs=103
sourceMap=1 visibilityMap=1
mapRequestedFrames=46 mapAttachedFrames=23 mapStaleFrames=23
mapVisibilityRejectedFrames=0 mapAttachRejectedFrames=0
```

UI 证据新鲜、地图可见，最近的 `game-state` 记录为 `focused=1`；
46 次绘制请求中有 23 次被源帧时效拒绝。

下层金库的 `events-20261014.jsonl:11438`，08:46:19：

```text
sourceAgeMs=121 sourceMaximumAgeMs=500 visibilityAgeMs=73
mapRequestedFrames=60 mapAttachedFrames=60 mapStaleFrames=0
mapVisibilityRejectedFrames=0 mapAttachRejectedFrames=0
```

另外首次大世界过滤存在一个 5177.33 ms 的冷启动异常长样本（08:43:27，行 8491）。
它在首个稳定窗口之外，不能代替后续每轮约 250–290 ms 的持续问题。

## 源码里的重复工作与尚未拆分的成本

计时边界在 `App::Start` 的 `filterMs`，包含投影中心转换、
`UpdateCenterPointNearItemsData` 的互斥锁等待、取得选中场景快照、过滤和更新状态。
日志已经确认这一块占整轮约八成；还没有独立计时每一个子调用，不能把 284 ms 全部
写成某一个 JSON 函数的净 CPU 时间。

具体代码结构：

1. `DrawItemOnGameMap.cpp` 的 `GetAndFilterItemsData` **每个选中类别**调用一次
   `DrawItemBase::GetFilteredPoints`，然后投影该类别所有点。
2. 非日刷类别调用 `MarkerCompletionStore::CompletedIds`。每次都重新将本场景
   `remoteIds` 装成 `std::set`，再遍历整个 `document.points`，最后复制成 vector。
   这个工作会随类别数和进度表规模重复。
3. 日刷类别调用 `FarmCompletionStore::CompletedIds`，同一场景的日刷集合也会
   为每个类别重复读取、遍历、排序。
4. 每个进入屏幕范围的点，又对完成 ID vector 做一次线性字符串搜索。
5. 上述结构对类别数、选中点数、账本大小、CPU 和锁竞争敏感，提供了不同玩家 /
   场景耗时差异的一条可验证解释。本附件不含完整的类别选择配置文件和进度账本，
   **不能给 even 的实际类别数或账本大小造值，也不能宣判它们各贡献了多少耗时。**

优先优化方向是一次批量读取所需完成状态、使用集合查询，并避免每类别 / 每帧重建
相同的集合。需要保持本地取消完成对云端基线的覆盖、日刷账本与长期进度的分离、
账户切换及即时更新语义；有缓存时必须覆盖全部失效来源。
再用现有日志验证过滤耗时、发布时年龄和 `mapStaleFrames` 是否一起下降。
渲染路径的单点完成状态读取也值得一并测量，但本次生产过滤是第一优先级。

本地 `v2026.9.9.6` 已含类似过滤循环和源帧寿命公式。新日志确认的是**当前故障的
机制与热点**，还没有重建 9.9.6 → 9.18.2 的同机器、同数据回归实验，不能宣判某个
历史提交就是回归原因。当前修复不需要等待历史回归逐提交定位。

## 复核与当前状态

执行 `python out/evidence/even-map-marker-flicker-20261014/analyze.py` 可重新生成
`analysis.json`、`qualified-diagnostics.json`。脚本核对每个窗口的更新设置 / 源帧寿命，
并断言各渲染结果之和等于请求总数，避免混用采样瞬时值和累计计数。
生产耗时是两秒抽样的一轮成本；渲染拒绝数是两秒累计迭代数，不能当作闪烁次数。

本轮仅登记新日志的分析与未完成修复项；源代码、测试断言、版本和测试包均未改动。
问题尚未修复，下一步应针对点位过滤做等价优化、回退对照和玩家复测。
