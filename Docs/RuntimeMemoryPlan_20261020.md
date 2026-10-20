# Runtime Memory Implementation Plan

> 执行方式：沿用当前分支和 inline executing-plans，按用户已批准的“归因→最大项优化→同场景验收”继续。

**Goal:** 找出资源初始化后新增内存，先消除可证明的匹配数据重复持有，保持特征、搜索参数、接受规则和地区选择不变。

**Architecture:** 独立进程探针测 SURF 与 FLANN 的工作集/private/峰值，显式统计可计算矩阵与索引字节。单个候选矩阵生命周期内直接构建同参数 KD-tree，持有 Mat 引用直到索引销毁，避免 matcher 合并副本与 miniflann clone。缓存按真实描述子、索引及关键点持有字节计费；光标线程不参与测量或释放。

**Tech Stack:** C++20/OpenCV 4.11/Win32 process memory；只使用本机代码与截图。

- [x] 探针及回归 RED：旧 FLANN 在一个不可变训练矩阵外还持有两份描述子；验证所有权、同 RNG 同参数的两个最近邻和距离。
- [x] 原生同参数单矩阵 matcher；查询维持 checks32/trees4/L2平方根与原 DMatch 行号。实测而非推测索引常驻字节。
- [x] 请求级 SURF/查询缓存、匹配索引计费诊断；探针分别测 SURF、训练及释放，避免并行阶段差值误称模块独占。
- [x] 真实资源、实际截图回放与原 matcher 同输入对照；确认缓存预算不会让全局 sweep 丢覆盖或反复重建。
- [x] CoreHost Release 与受影响回归、独立审阅；记录实际收益和仍未归因部分，不承诺600MB。

约束：不改变任何既有断言、匹配阈值、地区启用状态、楼层证据或 Direct 显示；不修改玩家安装。独立探针不读取或修改运行中的客户端。


## 实施与验证结果

### 可归因的内存

本机 OpenCV 4.11 源码显示，`FlannBasedMatcher::train` 先复制到 DescriptorCollection.mergedDescriptors，再由 miniflann Index::build 复制到 features_clone。原缓存只统计第一份描述子和 keypoints，未计这两份副本和 KD-tree。

独立新进程、80,000×128 float 同一输入：

| 阶段 | 原 matcher 工作集 | 单矩阵 matcher 工作集 |
| --- | ---: | ---: |
| 输入候选就绪 | 45.8 MiB | 45.4 MiB |
| 训练完成 | 144.4 MiB | 65.8 MiB |
| matcher 释放后 | 46.5 MiB | 45.9 MiB |

移除的描述子副本严格为 81,920,000 字节（78.125 MiB）；进程训练工作集减少约78.7 MiB，包含不同进程的小幅基线差异。新索引 reported usedMemory 为20,860,088字节，描述子仍为40,960,000字节。

整张2560×1440存档截图以SURF(100,4,3,true,true)在1/1.5/2/.75缩放下提取，2倍时进程峰值约893.4 MiB，销毁临时数据及SURF对象后约36.8 MiB。该探针使用整张截图，实际产品使用裁剪区域，因此不能把893.4 MiB直接当成产品某次调用的独占内存。探针表明这是需要继续处理的瞬时峰值，不能把所有增长都归为缓存或永久泄漏。本轮没有改变SURF参数或缩放覆盖。

九个场景按现有4MiB/64tile规则选取代表性窗口，含实际索引及关键点的总计费40,850,728字节（39.0 MiB），仍可容纳在默认48MiB预算内。这是按索引顺序取窗口的代表性探针，不声称所有实时排名组合完全同样大小；实际miss日志逐项报告descriptorBytes/indexBytes/entryBytes/heldBytes。

### 已落地的修复

- `OwnedFlannMatcher` 持有候选Mat引用，在同一数据上构建cvflann::Index<L2<float>>；trees4、checks32、L2平方根转换、DMatch行号保持原实现，描述子生命周期覆盖索引，禁止候选在索引活跃时改写。
- 缓存计入描述子、reported KD-tree pool/vind内存、keypoints容量。超过预算的条目供本次完整场景验证使用，完成/取消后释放，避免把一个大窗口长期留在缓存；不削减匹配候选。
- 增加前16次查询提取的阶段读数、查询矩阵字节和WGC双帧CPU字节/逻辑staging纹理大小；显式纹理大小不等于GPU或驱动驻留。进程级前后读数仍受其它线程影响，不能称独占分配追踪。
- `matcherCacheBytes` 加入结果诊断。实际所有者计数在真实index/Mat释放后减少，用于验证完成和取消路径均释放临时索引。

### 红绿与审阅

- 原FLANN包装：保留恰好一份描述子、单独报告索引字节两组断言RED；单矩阵实现GREEN。同RNG种子下2/128/4096行训练，所有最近邻行号和距离精确一致；调用方释放原Mat后仍可重复查询。
- 100,000行实际worker场景先RED：当前定位正确，但超预算数据仍留在缓存；修复后当前定位仍正确，cacheBytes=0。
- 独立审阅没有确定代码缺陷，指出缓存计数不足以证明临时索引释放。已补实际owner字节归零检查：正常完成和观察到索引实际构造后取消，均在结果发布前释放。临时去掉Worker末尾reset时两条生命周期断言RED；逐字节恢复后GREEN。
- CoreHost、VisualRegression、OwnedFlannTests和探针Release构建通过；受影响8/8原生回归通过（33.36秒），没有修改既有断言。保留原有数值转换和zlib CRT链接告警。
- 旧、新程序实际截图顺序回放均8/8通过：6个正例场景相同，2个负例均拒绝，正例坐标差最大0.101地图单位；未把两次序列输出称逐位相同。噪声负例的best-attempt诊断scene从1变0，均未接受或发布位置。单独在新进程回放0.5缩放正例，场景、坐标、203个matches、198个inliers和接受plan完全一致；全流程有预算进度、缓存和随机状态，不据一次计时声称CPU提升。

## 限制与后续

本轮证明了匹配器副本与超预算保留的问题并修复，尚未把整个约500MiB运行增量逐项全部归因，也未解决SURF大图瞬时峰值或保证实机低于600MB。缓存正确计费可能减少能缓存的大窗口，因此实机需同时观察重建频率、CPU及定位延迟。完整证据位于 `out/cpu-memory-analysis/post-deploy-20261020/`，含flann-before/after.csv、surf-before.csv、sweep-storage.csv、红绿日志、截图前后报告和pair comparison。
