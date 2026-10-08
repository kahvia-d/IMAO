# 视觉索引词汇表共享：方案与迁移风险

> 目标：把视觉索引里 14 份重复的词汇表降到一份，收益 **26.00 MiB**。
> 状态（2026-10-08）：**已落地**。分支 `feature/shared-visual-vocabulary`（堆叠在
> `feature/imf-v2-quantized` 之上，因为代码改动以那条分支的 v2 编码为基础），三个提交：
>
> | 提交 | 内容 |
> |---|---|
> | `a78ed4e` | 换检测器的决策实验文档（本轮之前的独立成果） |
> | `134aff1` | 共享词汇表：编码、运行时、构建器、三个探针、四个打包守卫、单测 |
> | `69388d2` | 14 个分片重打包 + `Map_visual_vocabulary.imx`（走 LFS） |
>
> ⚠️ **这是一次格式迁移**：新程序 + 旧包 ✅、旧程序 + 新包 ❌。程序与区域包必须同序发布，
> `New-ProgramReleasePackage.ps1` 已强制程序包必须带上共享词表。

## 一、为什么值得做

实测（2026-10-08，14 个区域包）：

```
区域包载荷合计                               215.8 MB
  其中视觉索引（visual-index.imx）           112.2 MB   ← 占 52%，是最大的一块
       vocabulary                            28.00 MB   ← 14 份完全相同的 2.00 MB
       histogram                             17.56 MB
       featureRow                            39.98 MB
       posting                               26.35 MB
       tile 表                                0.31 MB
  特征（features.imf）                        80.5 MB
  分层索引 + 参考图 + 清单等                    ~23 MB
```

- 14 个分片的 `vocabularySha256` **只有一个不同值**，即同一份词汇表被复制了 14 次。
- 词汇表**几乎不可压缩**：2,097,152 B → zlib-6 后 1,971,099 B（ratio 0.940）。
  所以这 28 MB 在**传输上**也是实打实的，不是解压后才会出现的账目数字。
- 去重后：索引 112.20 → **85.9 MB**（单份词汇表 + 其余不变），全量 215.8 → **189.5 MB**。

## 二、现状与约束

读 `IMao-Core/src/Feature/RuntimeFeatureRepository.cpp`：

- `MergeVisualShard`（`:25-66`）在**基索引没有词汇表时，取第一个分片的词汇表**。
- `:291-293` 的注释写明：**"分片是自包含的：没有选中基索引时，第一个合并进来的分片就是整个索引。"**
- 基础图集已退役（无基础大图布局），`Map_features.imf` / `Map_visual_index.imx` 不再随程序分发。
- 区域包**可以逐个删除**（想删就删，包括程序自带的副本）。

结论：词汇表**不能**放在任何一个区域包里——用户删掉那个包，其余包的定位就全废了。
它必须放在一个**程序级、永远存在**的位置。

## 三、设计

### 3.1 分片省略词汇表载荷，运行时从共享文件补齐

- 新增程序级资源 `Assets/FeaturesDatas/Map_visual_vocabulary.imx`（2.00 MB，永远随程序分发）。
  **它不是区域包**，所以不受"区域可删除"影响。
  （`Assets/FeaturesDatas/` 已经是程序级目录：`IconTask_Features.yml` 等一直住在这里。）
- 分片文件的 `vocabularyPayloadLength` 写 **0**，`vocabularySha256` **保持原值**。
- 运行时先加载共享词汇表，再把它传给每个分片的 `Load`。

### 3.2 关键：外部词汇表在 `Load` 里物化进内存

`Load` 拿到外部词汇表后，**把它填进 `output.vocabulary` / `output.vocabularySha256`**。
于是：

- `MergeVisualShard`、`cv::flann::Index` 的构建、以及所有下游**一行都不用改**；
- 分片在内存里的形状与今天**完全相同**；
- **磁盘变小，内存行为逐位不变。**

这是整个方案能做的前提——否则就得改合并与检索路径，风险立刻上一个量级。

### 3.3 用"重打包"而不是"重建"

**不要重跑索引构建。** 近期刚发生过一次索引重建导致识别质量崩塌（见
[`VisualIndexRebuildRegression_20261008.md`](VisualIndexRebuildRegression_20261008.md)）。

重打包工具只做一件事：读出分片，**删除词汇表载荷**，重写头部长度与 `payloadSha256`，
**tiles / histograms / featureRows / postings 四段逐字节原样搬过去**。
加上外部词汇表重新加载后，得到的索引必须与原文件**逐字段相等**——这一点由工具自己验证，
不靠人看。

### 3.4 校验不放松

- 没有词汇表载荷**且**调用方没有提供外部词汇表 → **硬失败**，错误信息明确指向缺失的共享文件。
  绝不退化成"静默降级、定位不工作"。
- 外部词汇表的哈希必须等于头部记录的 `vocabularySha256`，否则失败。
- 头部 `version`、几何、各段长度、文件总长、`payloadSha256` 的现有校验**一条都不删**。

## 四、迁移风险（我先前把它评为"低风险"，这个评级是错的）

这是一次**格式迁移**，需要**程序与全部区域包协同发布**：

| 组合 | 结果 |
|---|---|
| 新程序 + 新包 + 共享词汇表 | ✅ 正常工作 |
| 新程序 + **旧包**（自带词汇表） | ✅ 正常，旧路径保留 |
| **旧程序** + 新包（无词汇表） | ❌ 分片加载失败 → `visualIndexReady=false` → **定位失效**（日志有明确错误，但玩家只看到不定位） |

**旧程序那一格有直接证据，不是推测。** 旧版 `Load` 没有外部词汇表参数，它对"载荷长度为 0 的分片"
的行为**等价于**上面 E 项负向对照里"没提供词汇表"的那一次：14 个分片全部报
`visual index carries no vocabulary and none was supplied`，退出码 1。
（旧版代码里那条 `expectedVocabulary != header.vocabularyPayloadLength` 检查会以
`visual index payload lengths do not match counts` 拒绝，同样不会静默通过。）

因此发布时必须：

1. **程序与 14 个区域包在同一个发布里一起更新**（资源包与程序包同序发布，这是现有流程的能力）；
2. 共享词汇表必须进**程序包**（不是资源包），否则新程序装旧资源会缺文件；
3. `Test-BuildPrerequisites.ps1` 增加"共享词汇表存在"的前置检查；
4. `New-NoBaselineMapTestTree.ps1` 必须**保留**该文件（它现在会删掉 `Map_features.imf` 与
   `Map_visual_index.imx`，不要连带删掉词汇表）；
5. 发布说明里写明"本版起区域包需要新版程序"。

## 五、改动清单

| 位置 | 改动 | 状态 |
|---|---|---|
| `IMao-Core/src/Feature/VisualIndex/MapVisualIndex.{h,cpp}` | `Save` 增 `includeVocabulary`；`Load`/`LoadManifestShard` 增外部词汇表参数；新增词汇表文件读写 | ✅ |
| `IMao-Core/tools/VisualIndexRepack/` | **新增**：重打包现有分片 + 自验证 | ✅ |
| `VisualIndexBuilder/main.cpp` | 分片省略词汇表；构建时**总是**产出共享词汇表文件 | ✅ |
| `RuntimeFeatureRepository.cpp` | 先加载共享词汇表，再传给每个分片 | ✅ |
| `VisualIndexProbe` / `PackMergeProbe` / `VisualRegression` | 走同一条加载路径，自动发现共享词表 | ✅ |
| `OptimizationTests.cpp` | 往返一致、缺词汇表硬失败、错词汇表失败、旧包仍可加载 | ✅ |
| `Test-BuildPrerequisites.ps1` | 分片若省略词表则要求共享文件存在（读头部 `vocabularyPayloadLength`） | ✅ |
| `Build-IMao.ps1` | 源码树有共享词表就必须staged 到位 | ✅ |
| 仓库内 14 个分片的重打包 + 共享词表文件 | 已提交 `69388d2` | ✅ |
| `Refresh-MapTestBinaries.ps1` 等 map-test 树脚本 | 复核结论：**无需改动**——它只同步分层侧车数据与顶层二进制，不同步分片；运行根的 `Assets/FeaturesDatas` 来自构建产物，已由 `Build-IMao.ps1` 的守卫覆盖 | ✅ |
| `Stage-UpdateResources.ps1` | 复核结论：**无需改动**——`Assert-VisualIndexVocabulary` 读的是头部偏移 128 的 `vocabularySha256`，省略载荷后该字段照常写入，两棵树实测结果一致 | ✅ |

## 六、验证

### 已完成（2026-10-08）

1. **`IMaoOptimizationTests` 全绿**，含新增的 `TestSharedVocabularyShard`：
   - 去掉词汇表的分片**恰好**小一个词汇表载荷（2,097,152 B）；
   - 词汇表之后的**每一个载荷字节原样保留**；
   - 用共享词汇表重新加载后再序列化，**与原文件逐字节相等**；
   - 没有词汇表、也没提供共享词汇表 → **硬失败**且错误信息含 `carries no vocabulary`；
   - 提供了**不匹配**的词汇表 → 失败且含 `does not match`；
   - 共享词汇表文件的往返、损坏检测、缺失检测；
   - **旧路径不变**：自带词汇表的分片仍然不需要任何外部来源即可加载。
2. **真实 14 个包全部验证通过**（`IMaoVisualIndexRepack`，只读模式）：

```
avinoleum       3.08 →  1.08 MB   IDENTICAL      lowervault      3.62 →  1.62 MB   IDENTICAL
blackshores     3.28 →  1.28 MB   IDENTICAL      mengshutianluo  4.93 →  2.93 MB   IDENTICAL
darkplain       5.82 →  3.82 MB   IDENTICAL      mengzhou        8.79 →  6.79 MB   IDENTICAL
fabricatorium   4.31 →  2.31 MB   IDENTICAL      qiqiu           9.55 →  7.55 MB   IDENTICAL
jinzhou        19.65 → 17.65 MB   IDENTICAL      roysurface     10.03 →  8.03 MB   IDENTICAL
laguna          8.71 →  6.71 MB   IDENTICAL      tethys          2.90 →  0.90 MB   IDENTICAL
lahai          25.34 → 23.34 MB   IDENTICAL      timeriftruins   2.19 →  0.19 MB   IDENTICAL

每个包恰好省 2.00 MB，合计 28.00 MB；14/14 逐字节相等，0 失败。
```

### 待做

3. **仓库内 14 个分片的重打包 + 共享词表文件落地**（会改写 LFS 二进制，等分支确定）。
4. map-test 树脚本复核（`Refresh-MapTestBinaries.ps1` 等）。
5. 发布门禁：`Test-KuroMapFeaturePack.ps1` / `Stage-UpdateResources.ps1`。
6. map-test 实机确认定位仍然工作。

### 已完成的验证（逐项）

**A. 编码层** —— `IMaoOptimizationTests` 全绿，含新增的 `TestSharedVocabularyShard`：
去掉词汇表的分片**恰好**小一个词汇表载荷（2,097,152 B）；词汇表之后的**每一个载荷字节原样保留**；
用共享词汇表重新加载后再序列化**与原文件逐字节相等**；没有词汇表、也没提供共享词汇表 → **硬失败**且
错误信息含 `carries no vocabulary`；提供了**不匹配**的词汇表 → 失败且含 `does not match`；
共享词汇表文件的往返、损坏检测、缺失检测；**旧路径不变**（自带词汇表的分片无需任何外部来源）。

**B. 重打包安全性** —— `IMaoVisualIndexRepack`（只读模式）跑真实 14 个包，**14/14 IDENTICAL**，
每个包精确省 2.00 MiB。

**C. 净收益实测** —— 在临时树里复制 `FeaturesDatas`、重打包全部 14 个包、写入共享词表：

```
packs before      : 226,265,292 B  (215.78 MiB)
packs after       : 196,904,996 B  (187.78 MiB)
shared vocabulary :   2,097,212 B  (  2.00 MiB)
net saving        :                 ( 26.00 MiB)
```

**D. 运行时端到端** —— `IMaoPackMergeProbe` 跑重打包后的树：

```
shared vocabulary: loaded
14/14 shard=read merge=ok
total keypoints : 1176080      merged tiles : 6730      failures : 0
```

与**未改动树**的基线**逐项相同**（基线报 `shared vocabulary: absent`，走自包含旧路径）。

**E. 负向对照** —— 把共享词表临时移走后再跑：14 个分片**全部 FAILED**，错误明确指向
`visual index carries no vocabulary and none was supplied: <分片路径>`，退出码 1。
**没有静默降级。**

**F. 不变式探针** —— `IMaoVisualIndexProbe` 对**两棵树各 14/14 ACCEPTED**（自包含与共享词表两条路径）。

**G. 构建器端到端** —— 在临时树里补回归档的基础特征，真跑 `IMaoVisualIndexBuilder
--reuse-vocabulary`：复用词表 `8bd80ebd…`，**产出省略词表的分片**（每包约小 2.00 MiB）、
写出共享词表文件，该树 `PackMergeProbe` 同样 14/14 合并成功、6730 瓦片、0 失败。

**H. 打包守卫** —— `Test-BuildPrerequisites.ps1` 在**未改动树**上 PASSED（新检查是空操作）；
头部嗅探在真实文件上读出 `vocabularyPayloadLength` = **2,097,152**（原始）/ **0**（重打包与构建器产物）。

```powershell
# 只读验证（不写入仓库；加 --apply --vocabulary <path> 才真正重打包）
foreach ($p in (Get-Content Assets\FeaturesDatas\kuro-tile-packs.json -Raw | ConvertFrom-Json).packs) {
    & x64\Release\IMaoVisualIndexRepack.exe "Assets\FeaturesDatas\KuroTilePacks\$p"
}
```

## 七、新地图包（后续游戏更新新地区）

这一条单独写，因为它最容易在几个月后忘掉。

**结论：①（v2 量化）对新包没有影响；⑤（词表去重）引入过一个缺口，已修。**

### 新包会是什么格式

新包**天生就是 v2**，不需要任何迁移：`IMaoFeatureConverter` 的默认就是 v2
（`--format v1` 才写旧的 float32，那是为了复现更早的工具产物）。
① 只是把**已经存在**的 14 个包就地迁移了一次；新包走的是同一条转换器，直接落在 v2。

### 新包必须复用同一套词汇表

这是硬约束，而且是 ① 那一轮实测出来的：**词汇表不可复现**。固定种子
（`0x494d414f`）、固定采样、固定 FLANN 参数都没用——输入差半个量化步长就足以改变
k-means 的簇边界。所以新包的分片**必须**用现有那套词表（`8bd80ebd…`）来建，
否则新旧词表混装 → 合并失败 → 运行时**静默降级**（`visualIndexReady=false`，不报错）。

### ⑤ 引入并已修掉的缺口

⑤ 之前，"词汇表来源"可以是任何一个区域分片（它自带词表）。⑤ 之后分片不再带词表，
于是 `VisualIndexBuilder::ReadVocabulary` 的两条路都断了：

```
拿被剥掉词表的分片当来源 → "vocabulary source header does not describe a 4096x128 vocabulary"
拿新的共享词表文件当来源 → "vocabulary source is not an IMAOIX01 index"
```

只有归档的基础索引（`archive/retired-base-features/Map_visual_index.imx`）还能用——
一个把新包绑在退役归档上的隐式依赖。

**修法**（三处）：

1. `ReadVocabulary` **先试共享词表文件**（`IMAOVOC1`，走 `MapVisualIndexCodec::LoadVocabulary`
   的完整校验），再退回旧的 `IMAOIX01` 路径；并且当来源是"不再带词表的分片"时，
   错误信息**直接点名**应该传哪个文件。
2. `Build-VisualIndex.ps1` 的 `-VocabularySource` 默认值改成
   `Assets/FeaturesDatas/Map_visual_vocabulary.imx`（程序级、一直都在的那个）。
3. `--pack-only`（**新地区包走的就是这条**）现在自己保证共享词表存在且一致：
   文件不在就用当前基线的词表写出来，在就比对哈希，不一致**直接拒绝**。
   这样在一个从未跑过完整索引构建的树上也能建新包，而且新包不可能被建到另一套词表上。

实测：

```
--reuse-vocabulary <共享词表文件>   → reusing vocabulary 8bd80ebd… ✓
--reuse-vocabulary <被剥词表的分片> → 明确报错并指名 Map_visual_vocabulary.imx ✓
--pack-only                        → 产出 vocabularyPayloadLength=0 的分片，vocabularySha256 一致 ✓
--pack-only + 损坏的共享词表        → "does not match the baseline this pack was built against" 拒绝 ✓
```

### 加一个新地区时的完整清单

1. 下载瓦片（`Get-MapTileArchive.ps1`）→ 四点校准（`Set-KuroSceneCalibration.ps1`）
2. 抽特征 + 建包（`Sync-KuroMapFeaturePack.ps1` / `Invoke-MapRegionRebuild.ps1`）
3. **分片必须复用现有词表**（现在默认就会，且 `--pack-only` 会强制比对）
4. 参考图验证（8px 容差）
5. ⚠️ 登记 `kuro-tile-packs.json` **必须与 `approved=true` 同时发生**——
   `RuntimeFeatureRepository.cpp` 在快照模式下对"已登记但未获运行期批准"的包直接抛错
6. 打包门禁：`Test-BuildPrerequisites.ps1` 会检查"分片指名了共享词表就必须能找到它"；
   `Stage-UpdateResources.ps1` 会断言全部索引共用一套词表

**额外好处**：⑤ 之后每加一个新包少发 2.00 MB（不再自带词表副本）。

## 八、不做什么

- **不重建索引**（只重打包）。
- **不量化词汇表**——那会改变质心取值，进而改变词分配，属于刚刚踩过的那类质量回归。
- **不动词汇表大小**（4096 词不变）。
- **不放开任何现有校验**。
