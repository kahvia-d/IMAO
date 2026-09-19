# 区域按需下载 / 删除 · 设计说明（续篇）

本文是 `Docs/MapRegionRefactor-Handoff.md` 的**续篇**。前一份文档第四节的
"眼下的任务：做无基础库构建"**已经完成并在实机验证通过**，请勿再执行它。
本文记录之后的进展，以及下一件要做的功能。

先自证状态：

```powershell
cd C:\Dcode\WWMAP-TOOLS
git log --oneline -8      # 期望 HEAD = 973f852
git status --porcelain=v1 # 期望无输出
```

---

## 一、前一份文档之后发生了什么

| 提交 | 内容 |
|---|---|
| `3929baa` | 无基础库运行 + 修复"第一个包被算两遍" |
| `973f852` | 测试树不再保留包目录的旧拼写（大小写） |

### 1. 无基础库构建：**实机验证通过**

用户亲自在游戏里跑过、判断定位可用。因此：

- **遗留的融合图集（`Map_features.imf` + `Map_visual_index.imx`）可以退役**
- 程序包 981.9 MB → **831.6 MB**
- 先前设计的 `map-features` 包（"把基础库挪成可下载包"的折中方案）**不再需要**
- 为此落下的 `MapFeatureRoot` 管道（`222ace2`）现在是**惰性的、用不上的**。留着当"将来想把基础库做成可选下载"的接口，或撤掉；撤掉要动 native + managed 共 9 个文件。

判定依据（实测）：用 `scripts/Register-LegacySceneFeatures.py` 对 13 个区域包逐一配准基础库
（247,389 个关键点），并集 **218,615 = 88.4%**；未覆盖的 28,774 个点
**100% 落在已覆盖包围盒内部**（28,772 / 28,774，仅 2 个在外），是散布的孔洞，
不是漏掉的一整片地。八份报告在 `out/legacy-registration/*.json`（本地，被 gitignore）。

### 2. 找到并修复了一个会破坏定位的重复计数

无基础库路径新加的"从第一个包播种"逻辑把第一个包算了**两遍**（计数和追加各一次）。
证据是算术上的硬证据：合并关键点数 894,301，而 13 包 + candidate 应为 856,777，
差额 37,524 **正好等于第一个包 darkplain 的关键点数**。修复后实测 856,777。

副作用是**第一个包之后每个包的特征行基址偏移 +37,524**，视觉索引里记录的行号
指向错误的描述子。

### 3. 目录大小写问题

`New-MapTestTree.ps1` 算对了小写目标名，但 Windows 的 `CreateDirectory` **会静默复用
已存在的旧拼写目录**，于是被删掉的旧场景名（`Darkplain`/`Tethys`/`Lahai`/`RoySurface`/
`BlackShores`）一代代传下去。已修：快照里的包目录**逐段归一到磁盘真实名字**，
纯大小写改名走**临时名两步**（Windows 拒绝直接大小写改名）。

**严重性订正**：那份快照是**构建产物**（未跟踪），全新克隆由 `Stage-UpdateResources.ps1`
按 `kuro-tile-packs.json`（自 `dc5c9bb` 起全小写）生成，所以**不会破坏新克隆**——
这个缺陷只在本地树里自我延续。

### 4. 死目录已删

`Dreamzhou` / `DreamzhouWest`（232.6 MB 真目录 + 两个 junction）已从
`x64\Release\Assets` 与 `out\map-test\Assets` 清除，快照从未引用它们。

---

## 二、接下来要做的功能：玩家在 UI 里按区域下载 / 删除

### 为什么现在做不到

**约束 A（硬）**：客户端只能消费**签名快照**。`ResourceSnapshotService.Load` 要求
`FormatVersion == 2`，且 `UpdateSignature.Verify` 用内置公钥验签。所以客户端
**无法自己合成一个子集快照**交给 CoreHost——签名过不了。
"按需"不能靠"生成只含选中区域的快照"实现。

**约束 B**：运行时注册即加载。`RuntimeFeatureRepository.cpp:230` 会因**任何**已注册包
加载失败或未获运行时批准而中止整个资源加载（表现为"启动核心失败"）。

### 设计：把"可获得"与"已激活"分开

```
签名快照（updates/stable.json）    = 可获得什么（目录清单）
本地选择状态（unsigned，用户数据）  = 激活哪些（按 package id）
送给 CoreHost 的 Current            = 签名快照【过滤】到选择集
```

**关键好处：运行时一行都不用改。** 未选中的包根本不会出现在 CoreHost 收到的快照里，
约束 B 也就永远看不到它们。校验同理——`Load` 本来就是 `foreach (snapshot.Packages)`，
**先过滤即可**，不需要放宽任何校验。

### 强制保留、不可取消的包

| 包 | 原因 |
|---|---|
| `map-data` | `ResourceSnapshotService.cs:194` 要求 `Count(kind=="map-data") == 1`，且 `:203` 要求 `MapDataRoot` 等于它的目录 |
| `map-icons` / `map-features` | 对应根要能解析（若该布局用了它们） |

→ **可选的正好是 13 个区域包**，与"按区域按需"完全对应。

> **2026-09-19 更新**：原来这张表里还有一行 `dreamzhou-curated-locations`（候选包，7 MB），
> 现已**删除**——`mengzhou-kurotiles` 区域包取代了它。实测见
> `Docs/CandidatePackRedundancy.md`。随之删除的还有 `candidate-packs.json` 注册表、
> 两个候选包检查脚本，以及 `CandidateFeaturePack.cpp` 里按名字兜底加载它的那段。
> 因此第二节那个"候选包是否也允许取消"的问题**作废**。

### 两个必须处理的坑

1. **bundled 包的"删除"只能取消选择，省不出空间。** `FindBundledPackage` 会把随程序
   分发的包解析到安装目录（`Assets/...`）。删那些会破坏程序本身。删除动作**只能**针对
   `Root/packages/<id>/<version>`（下载来的副本）。
2. **删除必须拒绝强制包**，否则 `map-data` 一删，`Load` 直接抛"资源快照地图数据包无效"。

### 要动的文件

| 层 | 文件 | 改什么 |
|---|---|---|
| 契约 | `IMao-WinUI.Core/Updates/UpdateContracts.cs` | 本地选择状态（按 `snapshotId` 持久化） |
| 服务 | `IMao-WinUI.Core/Updates/ResourceSnapshotService.cs` | `Current` 按选择过滤；`Rebind`/`MapDataRoot` 仍从强制包推导 |
| 服务 | `IMao-WinUI.Core/Updates/UpdateService.cs` | `EnsureInstalledAsync(packageIds)`（只装子集）、`RemoveAsync(packageIds)`（取消选择 + 删下载副本） |
| UI | `IMao-WinUI/Views/SettingsPage.xaml(.cs)` | 区域列表：名称 / 大小 / 状态（内置·已下载·未下载）/ 下载·删除 + 总下载量 |
| 测试 | `Tests/ResourceUpdates`（已有 `RealOfflineRunner`） | "选两个区域 → 只下这两个 → 删一个 → 快照仍可加载" |

### 实现顺序（每步独立可验证）

1. ~~**选择状态 + 快照过滤**（纯 Core，无 UI）~~ —— **已完成**（`6c33e4c`）。
2. ~~**`EnsureInstalledAsync` / `RemoveAsync`**~~ —— **已完成**，见下节。
3. ~~**UI 列表 + 按钮**~~ —— **已完成**（见"第 3 片落地说明"）。界面效果仍需用户在实机确认。
4. **实机验收**：取消选中某区域 → 重启 → 该区域不再加载；重新选中 → 只下载它一个。
   线上渠道已就绪（sequence 14 提供全部 13 个区域包），因此"重新下载"这一半现在可测。

### 第 2 片落地说明（实测中改掉的设计）

- **快照只能命名"磁盘上确实存在的包"**。原生加载器看到任何一个包加载失败就拒绝整份快照，
  所以子集安装必须把快照收窄到已安装的集合；而"哪些能装"的权威是**发布清单**，不是快照。
  `EnsureInstalledAsync` 因此以 release 为准校验请求，并把新装上的区域
  `AttachPackagesAsync` 进当前快照。
- **移除不改写描述符，只删副本 + 记录选择**。`RemoveAsync` 删 `Root/packages/<id>/<version>`
  与对应 receipt，并在 `selection.json` 记录取消选择；描述符保留该包，所以能再装回来。
  程序自带的副本**只取消选择、不删**（删了既破坏安装又省不出空间）。
- **必需包会自动装上**。调用方只请求区域；`map-data` 这类必需包若本地缺失会被一并安装，
  否则快照无法加载。
- **首次安装是"待启用"的**。`EnsureInstalledAsync` 之后 `Current` 仍是原快照、
  `HasPending=true`，与既有的资源更新一致：下一次启动验证新快照并回报健康后才采用。
- **修了一个既有缺陷**：bundled 描述符的 `sha256`/`files` 按设计为空，而 `FindBundledPackage`
  要求它们匹配，于是"程序自带的包"永远匹配不上——`ShipsWithProgram` 永远返回 false，
  且每个自带包都会被当成缺失而重新下载。现在空哈希按 id/版本/类型匹配。

### 第 3 片落地说明（列表、开关、删除）

- **列表不依赖"检查更新"**。区域的**成员资格**来自本机快照（`AvailablePackages()`），
  是安装事实，离线就知道；发布清单只回答"能不能下载 / 有没有新版本"。
  `RegionCatalog.Build` 因此接受 `ResourceRelease?`：没有清单也照样列出全部区域，
  只把尺寸改成"本机副本大小"而不是"下载大小"。`UpdateUiController.Regions()` 不再返回空表。
- **开关不再闪烁**。原因是每次 `PropertyChanged`（核心加载进度、下载进度每一跳）都
  `Children.Clear()` 重建整个列表。现在行对象按 `PackageId` 缓存复用，只在**区域集合变化**时
  重建；文本/开关就地更新。另外缓存 400 ms 内的区域数据、安装过程中不重算尺寸
  （尺寸要遍历磁盘），玩家刚拨动的开关在操作结束前不会被"回弹"。
- **"本机有副本"才算内置**。玩家删掉自带副本后，行必须显示**未安装**（`Directory.Exists` 判定），
  否则会显示一个不存在的副本且体积为 0。
- **启用优先用本机副本**。`EnsureInstalledAsync` 对"发布清单里没有、但本机磁盘上有副本"的请求
  不再报错，而是直接把它移出 `Deselected` 并刷新宿主快照——所以**自带区域停用后再启用
  完全不需要网络**。反之，若本机副本已被删除且清单也不提供，则明确报错而不是把宿主
  指向一个空目录（那会让整份资源加载失败）。
- **删除按钮的可用性 = 本机是否有副本**。区域包想删就删：删掉之后重新启用会重新下载
  （发布渠道已同步，见第五节）。判定"能不能再拿回来"曾经被写进 `Deletable`，
  但玩家关心的是磁盘空间，所以现在只看本机副本是否存在。

### 默认选择（已拍板：方案 A）

**方案 A = 区域包继续随程序分发，默认全部生效。** 这是最终决定。

`PackageSelection.Deselected` 用 **`null` = 玩家还没选过**，此时可选包**全部生效**；
非空列表 = 玩家关掉的那些区域。

为什么默认全选而不是全关：

- **随程序分发的副本不产生下载**。13 个区域包就在程序包里，所以"默认全选"下玩家的
  **区域下载量是 0**。
- 真正会下载的只有**程序不自带的区域**（发布清单里版本比自带的新）。这种"默认不下载"
  已经由"未选中不下载"保证：不选就不下。
- 默认全关会让首次启动**没有任何区域可定位**，必须先去 UI 里点一下——对现装用户是倒退。

交互上 UI 仍然把 13 个区域列出来，玩家**取消勾选 = 不再加载**；若该区域是下载来的，
取消时可一并删除其下载副本，重新勾选只下它一个（实测 1 次请求）。
**随程序的副本只取消选择、永不删除**（删了既破坏安装又省不出空间）。

#### 被否掉的方案 B（记录备查）

**B = 把 536 MB 区域包移出程序包，改成真正的按需下载，默认全不选。**
程序包 981.9 MB → 约 445 MB（若再退掉 150.3 MB 基础库），收益很大，但代价是
**首次启动没有任何地图可用，必须先下载**。用户选择 A，故不做。
哪天要缩程序包，B 是可以直接捡起来的——第 1、2 片的机制（收窄快照 + 子集安装）
正是它需要的全部基础，届时只需让 bundled 快照不再列出区域包。

### 需要用户拍板的一个点

- ~~**候选包（`dreamzhou-curated-locations`）是否也允许取消？**~~ —— 该包已删除，此问题作废。
- ~~**默认选择是"全部 13 个区域"，还是只有某一个子集？**~~ —— 已定：默认全部生效
  （见上节"默认选择"）。

---

## 三、其余未决事项

- **基础库退役的落地**：删掉 `Assets/FeaturesDatas/Map_features.imf` 与
  `Map_visual_index.imx`，并把 `New-ProgramReleasePackage.ps1:101` 里"程序包必须包含这两个
  文件"的强制要求去掉——**这一步才是字面意义上的"不再打进程序"**。
- `MapFeatureRoot` 管道是否撤掉（见第一节 1）。
- 13 个区域包的窗口重叠问题（早先实测：帧 8 六个包共声明 481 个瓦片引用而实际只有 321 个
  唯一文件，黑海岸 0 张独占）——不影响正确性，但 `map-data` 之外的下载量可以再优化。

---

## 四、环境与命令

见 `Docs/MapRegionRefactor-Handoff.md` 第六、七节（工具链路径、构建命令、原生预检的写法、
配准脚本的代价模型）。要点复述：

```powershell
# 重编
cmd /c "call C:\VSBuildTools-Current\VC\Auxiliary\Build\vcvars64.bat && cmake --build out\build\windows-x64-release --target IMao-CoreHost IMaoResourceSnapshotTests"
# 测试：期望 58 checks, 0 failed
x64\Release\IMaoResourceSnapshotTests.exe
# 交付给用户实机跑的树
out\map-test\IMao-WinUI.exe      # 以管理员运行、游戏开着、16:9
```

`out/diag-nobase-snapshot.json` 与 `out/diag-withbase-snapshot.json` 是排查时用的
**绝对路径 strict 候选快照**，可直接拿来跑 `--check-resource-snapshot`。
`RuntimeFeatureRepository` 现在会通过 `Diagnostics::Record` 记录
`stage=visual-index-composition`（各场景瓦片数），这是排查"某个场景没进索引"的第一手信息。

**刷新 `out\map-test` 时只拷这两个文件**：`Assets/Updates/bundled-snapshot.json` 与
`trusted-keys.json`。**绝不能整目录拷 `Assets/Updates\*`**：那棵是无基础库（no-base-atlas）
测试树，`scripts/New-NoBaselineMapTestTree.ps1:176` 明确要求
`Assets/Updates/baseline-files.json` **不存在**——它一旦出现，原生校验就会去检查
被刻意删掉的 `Map_features.imf` / `Map_visual_index.imx`，整份快照以
`resource file missing or uses reparse point` 被拒（区域选择检查随之变红）。
这是本次实测踩到的坑：树本身没问题，是刷新方式错了。

---

## 五、发布 2026.9.19.1（区域重构必须随程序发布）

区域重构**不能只发资源**：新布局要新的 CoreHost 与新的区域注册表，而线上程序还是
`f8a3f7b`（区域重构之前——图标还在 `map-data` 里、区域包还是旧的 7 个，
而程序版本 `2026.9.18.2` 已发布且不可变）。所以必须抬版本号、重发程序，
资源清单同时重发：`min-app-version` 指向新程序版本，旧客户端根本看不到它，
也就不会被喂它不认识的布局。

发布链路上被这次重构打断、已修好的三处（都是同一类：构造绝对快照时漏了可选根）：

1. **`UpdateSignature.ValidateCatalog` 不认 `map-icons`**。白名单只有
   `map-data|tile|candidate`，而 staged 快照里**始终**带着图标包，
   于是 `prepare` 直接报"资源包标识重复或类型不受支持"——**任何资源更新都发不出去**。
   现在白名单加上 `map-icons`（`RequiredKinds` 早就把它当作快照可携带的类型，
   快照侧也一直在校验图标根），发布器自测里那份镜像校验同步更新。
2. **`scripts/Test-ProgramReleasePackage.ps1`（程序包验收探针）与
   `scripts/Restore-BundledResources.ps1` 漏了 `mapIconRoot`**。原生 `Root()` 要求绝对路径，
   相对值会让整份快照以 `invalid resource directory: mapIconRoot` 被拒——
   程序包验收会因此失败。已按"可选根也要绝对化"统一处理（含 `mapFeatureRoot`）。
3. **`tools/UpdatePublisher/Program.cs` 的 `prepare` 预检快照同样漏了它**。
   这一处是真正卡住发布的那一处：包已经全部打好、清单已经签好，
   最后卡在原生 strict 预检（`nativePassed=false`）。同一个 `Absolute()` 处理。

私钥位置（`Docs/ResourceUpdates.md:33-51` 描述的那个重定向坑，实测就在这里）：

```
C:\Users\Kahvia\AppData\Local\Packages\OpenAI.Codex_2p2nqsd0c76g0\LocalCache\Local\WWMAP-TOOLS-Publisher\release-signing-key.json
```

实测它在本机 DPAPI（当前用户）下可解开，且与 `Assets/Updates/trusted-keys.json`
里的公钥逐字节匹配，因此生产签名可用。

### 已发布：sequence 14 / v2026.9.19.1

`2026.9.19.1` 已经作为**程序 + 资源同发**上线：

- 程序包 `IMao-v2026.9.19.1-windows-x64.zip` 881,343,690 字节，构建提交 `0aa9873`
  （`Build-ReleaseCandidate.ps1` 全新原生重编 + WinUI 发布 + 程序包验收）。
- 资源 `resources-2026.9.19.1`（sequence 14，`min-app-version` = 2026.9.19.1）：
  `map-data` + `map-icons` + 13 个区域包 = 15 个包，离线包 495,032,405 字节。
  旧布局（`dreamzhou*`、已删除的候选包）随同一 baseline 的快照一起退场。
- 发布校验：18 个附件逐个按 GitHub 报告的 digest 与复核哈希比对，再对每个公开 URL
  做 HEAD 可达性检查；`updates/channel-state.json` 先烧序号（maxSequence=14），
  再用 compare-and-swap 推进 `updates/stable.json`，回读字节与签名清单哈希一致。
- 线上清单用**随程序发布的公钥**独立验签通过：`sequence 14`、`appVersion 2026.9.19.1`、
  `snapshotIds [resources-2026.9.19.1]`。
- tag `v2026.9.19.1` 指向构建提交 `0aa9873`；它之后的三个提交是两处测试/工具修复
  加一处行为等价的 `MapFilterCatalog` 重构（生产调用点不传新参数），
  因此 tag 与 main 的差异对程序行为没有影响。

### 交付给实机验收的树

`out\map-test` 已刷到 2026.9.19.1（二进制、`Assets/Updates/bundled-snapshot.json`、
原生 CoreHost 全部对齐；`Assets` 下的 `KuroMap`/`KuroMapIcons` 等仍是到 `x64\Release` 的
联接）。**12 个区域包仍是目录联接**，所以对它们点「删除」会被
`UpdateStorage.RejectLink` 拒绝（"资源目录不能包含符号链接或目录联接"）——这是保护源数据的
设计，不是缺陷。为了让"删除 → 重新下载"能在实机跑通，**特地把 `tethys` 换成了真实拷贝**：
对「泰缇斯之底」删除 → 变未安装 → 重新启用会从线上只下载 `tethys-kurotiles` 这一个包。

### 实机反馈修掉的两个缺陷（`4f99d7e`、`d7f8c83`）

2026.9.19.1 发布后实机第一次跑"删除 → 重新启用"就暴露了两个**由第 3 片改动引入**的缺陷：

1. **删掉自带副本后重新启用会假装装好**。`FindBundledPackage` 只匹配 bundled 描述符的
   id/版本/类型，**不看副本是否还在**，于是"程序自带、无需下载"依然成立：写入了已启用、
   跳过了下载、界面永远停在「未安装」。修复：匹配必须要求目录存在（`Directory.Exists`），
   这正是"删掉的自带区域会重新下载"的依据。
2. **同一份快照会被激活一个不存在的目录，CoreHost 秒退**。启用把包激活进宿主快照，
   而该目录已被删除；原生 `Root()` 要求目录存在，于是 CoreHost **在写下任何日志之前**
   就退出（`resource snapshot rejected: invalid resource directory: directory`，
   退出码 5），界面停在「正在启动 CoreHost」，窗口也关不掉。修复：
   `ApplySelection` 收窄宿主快照时**剔除副本已消失的可选包**（必需包不参与），
   让"选择"永远不可能拖垮整份资源加载；界面如实显示未安装。

第 2 条还牵出一个隐藏更深的：重新下载会把包从程序目录搬到 update 根目录，
`StageAsync` 按**字节**比较同一快照标识，于是以"同一资源快照标识已存在不同内容"拒绝。
现在改为比较**签名内容**（忽略本机路径），只在路径变化时重写存储描述符。

顺带把"卡住关不掉"这件事本身治了：`CoreHostService` 等待管道连接时会**与进程退出赛跑**，
进程一死立刻以退出码报错（不再等满 180 秒），并把 CoreHost 的 stderr 持续写进
`Logs/corehost-stderr.log`（原来那条 `resource snapshot rejected: …` 谁都看不到）。

两条回归测试钉住了 1 和 2（`Tests/ResourceUpdates`，修之前必红）；全量门禁：
托管 78/78、区域选择原生 `ready=True`、发布器 19、来源 13、staging 7、目录迁移 23。

**待办（用户已定）**：等实机验证通过后再补发 **2026.9.19.2**（修复只动托管代码与测试，
原生与 13 个区域包字节未变，可沿用旧 URL，只需重传程序 ZIP 与离线包 + 推进 sequence 15）；
**不做**"启动时自动补下缺失区域"，保持手动关掉再打开。

### 第三轮实机反馈（`0ba18fe`）：快照里的"位置"没跟着搬

用户报"下载过了、重启后仍显示未安装，再点下载也不再下载"。根因不在下载，而在
**`AttachPackagesAsync` 只追加它还不认识的包，从不更新已存在包的位置**：

- 删除自带副本后重新下载，字节从**程序目录**落到 **update 根目录**；
- 但快照里那一条还是老路径（程序目录），于是界面按 `Directory.Exists(老路径)` 判定
  **未安装**；而再点一次下载时 `FindBundled` 为 null、update 根已存在 → 只校验不下载，
  所以"看起来下过了、却没有生效"。宿主侧则由上一轮的 `ApplySelection` 兜住（剔除缺目录的包），
  所以核心能起来，只是那一行永远显示未安装。
- 修复：`AttachPackagesAsync` 对已在快照中的包**替换其 `Directory`**（仅本机路径变化，
  签名身份/哈希/文件清单不变；`StageAsync` 的"同一快照标识"比较已按签名内容判断，配合得上）。
- 钉住它的测试：`the region list reports a re-downloaded region as downloaded, restart included`
  —— 直接读 UI 的数据源 `RegionCatalog`，走"自带 → 删除 → 下载 → 重启"并断言每一步的
  `RegionState`（修之前必红：下载后即报 NotInstalled）。

同一轮还改了两处交互/测试：

- **未安装的行改为显示「下载」按钮**（原来是「删除」，且因为行缓存复用，状态变了按钮不跟着变）。
  现在按钮在**每次渲染**时按状态决定：有副本→「删除」，未安装且可下载→「下载」；
  点「下载」= 下载并启用该区域（不必再靠"关掉再打开开关"）。
- `RegionSelectionCheck` 在 staged 树缺包时**明确报出缺哪个包**：实机上删过区域后跑测试，
  以前会在目录拷贝里抛 `DirectoryNotFoundException`，看起来像检查本身坏了。
