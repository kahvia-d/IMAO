# 程序本体分片增量更新方案（已评审）

> 状态：**方案，已评审，未实现**。用户 2026-09-20 要求先看方案再决定实现；同日评审通过，
> 决策见 §13.1，仍未决项见 §13.2。文中标「评审修正」「评审新增」的部分是评审时对着代码改过的结论。
> 相关代码：`IMao-WinUI.Core/Updates/ProgramUpdateStore.cs`、`ProgramPackageValidation.cs`、
> `UpdateContracts.cs`、`UpdateSignature.cs`、`UpdateService.cs`、
> `tools/UpdatePublisher/Program.cs`、`scripts/New-ProgramReleasePackage.ps1`、
> `scripts/Stage-UpdateResources.ps1`、`scripts/Publish-ResourceUpdate.ps1`。

## 1. 问题

程序更新目前是**整包下载**：

- 清单里程序是单个 `ProgramPackage { Url, Size, Sha256, Files[] }`，指向 GitHub Release 上的
  一个 `IMao-v<version>-windows-x64.zip`。
- 客户端 `ProgramUpdateStore.PrepareAsync` 的动作是
  `download(package, stream)` → 落盘 `program.zip` → `ExtractAsync` → `VerifyDirectoryAsync`
  → 原生 preflight → 原子切换。
- 因此**每次程序更新都要下整包**：当前 `2026.9.19.1` 是 **840.5 MB**（解压后 1329.6 MB）。

资源包侧**已经**是差分的（内容不变的包保留旧版本与旧 URL，只上传变化包），程序侧没有。

## 2. 实测体积构成（决定分片怎么切）

`out/release-candidate-2026.9.19.1/program/IMao-v2026.9.19.1-windows-x64`，解压后 1329.6 MB：

| 类别 | 大小 | 文件数 | 每次程序更新会变吗 |
|---|---|---|---|
| 可执行与 DLL（自包含 .NET 运行时 + Paddle/mklml/mkldnn/OpenCV/vcomp 等） | 537.6 MB | 319 | 基本不变 |
| `Assets/FeaturesDatas/KuroTilePacks`（13 个区域包） | 535.9 MB | 83 | 基本不变 |
| 基础库 `Map_features.imf` + `Map_visual_index.imx` | 150.3 MB | 2 | **已归档退役，不再打包** |
| `Assets/KuroMapIcons`（图标包） | 34.9 MB | 528 | 偶尔变 |
| `Assets/models`（OCR 模型） | 22.9 MB | 10 | 几乎不变 |
| `Assets/其他` | 22.7 MB | 11 | 偶尔变 |
| `Assets/KuroMap`（点位/状态/攻略） | 16.3 MB | 33 | 偶尔变 |
| 其他（清单、许可、启动器配置…） | 8.9 MB | 241 | 偶尔变 |
| **其中项目自身产物** | **≈38 MB** | | **就是这部分在变** |

项目自身产物明细：`IMao-WinUI.exe` 0.42 · `IMao-WinUI.dll` 1.1 · `IMao-WinUI.Core.dll` 0.22 ·
`IMao-CoreHost.exe` 35.0 · `KuroSyncBridge.exe` 1.06 MB。

**结论：一次典型程序更新真正变化的通常只有几 MB，而现在要下 840 MB。**

## 3. 关键判断：增量与"区域包随程序分发"不冲突

区域包（535.9 MB）、自包含运行时与第三方 DLL（537.6 MB）几乎不随版本变化，**文件级比分会直接把
它们跳过**。所以：

- 不需要为了省流量去改成"区域包按需下载"（方案 B）。决策 A（开箱即用）可以保留。
- 区域包与 `map-data`/`map-icons` **本来就已由资源更新渠道管理**（可选、可删、可差量更新）。
  程序分片只需覆盖**程序本体**，避免同一份字节被两套机制重复携带与重复下载。

## 4. 分片怎么切

分片按"**变化频率**"切，不按目录切；片数控制在个位数，避免附件与请求数膨胀。
下表是**实测值**（2026.9.19.2 真实暂存根），由 `shard-map` 与一次真实 `prepare` 干跑得到，见 §4.2：

| 分片 | 内容 | 解压 | 压缩后（实际下载） | 文件数 | 变化频率 |
|---|---|---|---|---|---|
| `runtime` | 自包含 .NET 运行时、`paddle_inference.dll`、`mklml/mkldnn/libiomp5md`、`opencv_world*.dll`、CRT、`Microsoft.*`、locale 资源目录、`Microsoft.UI.Xaml/`、`NpuDetect/` | 442.0 MB | 142.2 MB | 536 | 换 .NET/依赖时 |
| `core` | `IMao-CoreHost.exe`、`common.dll` | 35.6 MB | 25.7 MB | 2 | 改 CoreHost 时（该 exe 无版本资源） |
| `ui` | `IMao-WinUI.exe/.dll/.Core.dll`、`.deps.json`、`.runtimeconfig.json`、`KuroSyncBridge.exe`、`IMao-Launcher.exe`、`launcher-build-info.json`、`build-info.json`、`appsettings.json`、`resources.pri`、`LICENSE`、`ProgramUpdates.md`、`README-Updates.md`、`Licenses/`、`Assets/Updates/` | 69.0 MB | 29.8 MB | 24 | **每个发版都变**（见 §10） |
| `assets-misc` | `Assets/models`、`Assets/Fonts`、`Assets/th.jpg`、`Assets/WindowIcon.ico` | 45.3 MB | 34.0 MB | 13 | 偶尔 |
| `assets-map-data` | `Assets/KuroMap`（点位/状态/攻略/校准/准入） | 16.4 MB | 1.6 MB | 33 | 偶尔 |
| `assets-map-icons` | `Assets/KuroMapIcons` | 34.9 MB | 33.6 MB | 528 | 偶尔 |
| `assets-tiles` | `Assets/FeaturesDatas/`（`KuroTilePacks/*` 及同目录的注册表 json 与散装 yml） | 536.2 MB | 437.0 MB | 86 | 基本不变 |
| **合计** | | **1179.4 MB** | **703.9 MB** | **1222** | |

（当前线上 `2026.9.19.1` 的整包 zip 是 840.5 MB，但那一版还带着已退役的基础库；
同口径（`.19.2` 暂存树）的整包预计与上表合计相当，见 §10 的口径说明。）

`assets-*` 三片与资源渠道的包**同源**：同一份文件清单已经在 `ResourcePackage.Files` 里。
决策是让程序分片照旧携带它们（§8 的 A1），跨渠道引用留作后续。

### 4.1 分片边界的硬规则（评审新增）

1. **每个发版都会变的文件必须落在 `ui`**：`build-info.json`（`VerifyDirectoryAsync` 要求它的
   `AppVersion`/`BaselineId`/`SourceCommit` 与签名清单一致，所以它**每个版本都变**）、
   `launcher-build-info.json`、`Assets/Updates/*`、以及所有带版本资源的项目产物。
   **发布器断言：这些路径全部属于 `ui` 片。** 一旦搞错，每次发版都会连带重下 `runtime`
   （442 MB），分片收益直接归零。
2. **`IMao-Launcher.exe` 放 `ui`**，并接受它带来的后果：该 exe 带 `FileVersion = appVersion`
   （`New-ProgramReleasePackage.ps1:40` 强制要求），所以它的字节**每个发版都变**，`ui` 片因此
   每次发版都要重下 ≈69 MB。这是当前打包约定的直接结果，不是分片表的问题；压窄它的办法见 §10。
   启动器协议变更时 `ProgramUpdateStore.cs:79` 本来就会拒绝自动更新（提示手动安装整包），
   分片不改变这个语义。
3. **映射用"有序路径规则 + 未识别即报错"实现**，不用目录硬编码，也不是"静默兜底片"：
   新增一个 `Assets/*` 子目录或一个 `IMao-*.dll` 若没有被显式分类，发布器**直接失败并列出路径**，
   而不是让它悄悄落进 442 MB 的 `runtime`。`runtime` 只按位置接收第三方/运行时文件
   （根目录下不以 `IMao-`/`Kuro` 开头的文件、locale 目录、`Microsoft.UI.Xaml/`、`NpuDetect/`）。
4. **客户端只校验结构、不认片名**：①分片是 `Files[]` 的完整划分（不重不漏）；
   ②片标识唯一、片内文件非空、片元数据合法（GitHub 下载地址、`0 < Size < 2 GiB`、合法哈希）；
   ③分片数量上限 16。片名语义（哪片装什么）是发布器的规则，见规则 1–3。
5. **（可选，v1 不做）** 把 `runtime` 再拆 `runtime-dotnet` / `runtime-native`，把"换 .NET 或
   Paddle 要下 442 MB"这个尖峰压到百兆级。§11 已承认分片边界后调只是相关片重下一次，
   所以先按上表落地，等真的遇到 .NET/Paddle 升级再说。

### 4.2 已实现的入口（阶段 1–2）

- `tools/UpdatePublisher/ShardMap.cs`：上面这张表就是代码里的唯一一份规则（有序匹配）。
- `dotnet UpdatePublisher.dll shard-map --app-root <暂存根> [--report <json>]`：在真实暂存根上跑
  分类，未识别文件即失败；打印每片文件数与字节数，`--report` 落一份机器可读报告。
- `dotnet UpdatePublisher.dll prepare --program-release true --program-shards true ...`：真实产出
  分片（见 §9）。阶段 2 干跑（测试密钥，不发布）：

  | 项目 | 结果 |
  |---|---|
  | 暂存根 | `out/release-candidate-2026.9.19.2/program/IMao-v2026.9.19.2-windows-x64` |
  | 分片 | 7 片，压缩后 **703.9 MB**（解压 1179.4 MB / 1222 文件） |
  | 描述符 | `IMao-v2026.9.19.2-shards.json`，**2883 字节**（旧客户端只会下到它然后拒绝该版本） |
  | 重组验收 | 7 片解压回 `program-reassembly/`，**1222 文件 / 1179.4 MB**，通过原有 `VerifyDirectoryAsync` |
  | `verify` | 7 片 + 描述符 + 15 个资源包全部通过 |
  | 证据 | `out/shard-phase2/real-prepare/`、`out/shard-phase1/real-app-root-shards.json` |

## 5. 清单格式（向后兼容）

在 `ProgramPackage` 上新增分片数组，**保留**现有 `Url/Size/Sha256/Files` 作为整包回退：

```csharp
public sealed record ProgramShard
{
    public string Id { get; init; } = "";                     // runtime / core / ui / assets-*
    public string Url { get; init; } = "";
    public long Size { get; init; }                            // zip 字节数
    public string Sha256 { get; init; } = "";
    // 评审修正：只存路径，不重复 size/sha256。Files[] 才是权威清单，
    // 见 §5.2 的清单体积理由。
    public List<string> Files { get; init; } = new();
}

public ProgramPackage { ... 现有字段不变 ...; public List<ProgramShard> Shards { get; init; } = new(); }
```

### 5.1 定位：`Files[]` 是契约，`Shards[]` 只是字节怎么运

- `Files[]` 仍然是权威且必须完整：`ValidateInstalledAsync` → `VerifyDirectoryAsync` 靠它逐文件
  校验整棵已安装目录，`ProgramPackageValidation.Validate` 还要求它包含 `RequiredFiles`。
- `Shards[]` 只回答"某个文件从哪个 zip 来"。分片必须是 `Files[]` 的**完整划分**（不重不漏），
  发布器与客户端都断言这一点。
- 因此整包字段与分片字段是同一份文件清单的两种视图，不会各自漂移。

### 5.2 分片里只放路径（评审修正）

原方案让 `ProgramShard.Files` 重复整份 `path/size/sha256`。问题：

- 清单体积翻倍。`UpdateSignature.MaxManifestBytes = 8 MB` 是硬上限，而 `Files` 的上限是
  30000 条；当前 app 只有 ~1228 个文件（约 200 KB 级），但最坏情况会撞上限。
- 重复本身就是漂移来源。片内只放路径（或索引区间），`size`/`sha256` 从 `Files[]` 查表，
  "完整划分"的断言也更简单（路径集合的并集等于 `Files[].Path`，且两两不相交）。

### 5.3 老客户端兼容的正确说法（评审修正）

原方案说"同一份清单可以同时服务新老客户端，一次发布即可"。这话**只对了一半**，实测依据：

- `UpdateSignature.ValidateCatalog`（`UpdateSignature.cs:43`）会整体调用
  `ProgramPackageValidation.Validate(catalog.App.Package)`，所以整包字段必须**始终合法完整**：
  `Url` 必须是本仓库 `/releases/download/` 形状、`0 < Size < 2 GiB`、`Sha256` 合法、
  `Files` 含全部 `RequiredFiles`、无重复/前缀冲突路径、展开 ≤ 8 GB。
- 老客户端**不会**因为未知字段 `Shards` 而失败（`System.Text.Json` 默认忽略）。
- 好消息：整包附件 404 **不会**卡住资源更新。`CheckAsync`/`InstallAsync`（资源）与
  `PrepareProgramAsync`（程序）是两条独立路径，清单校验不检查附件是否存在。
- 坏消息：老客户端点"更新程序"时会下载失败。所以必须二选一，不能含糊：

| 选项 | 做法 | 代价 |
|---|---|---|
| **B（已定，用户 2026-09-20 确认）** | 分片发布不再上传整包 zip；把 `Url/Size/Sha256` 指向一个**真实存在的小文件**（如分片清单 JSON），使老客户端在 `Content-Length` 预检（`UpdateService.cs:83`）就快速失败 | 老客户端无法自动升级程序，只能手动装整包；不浪费流量、不动旧版本、资源更新照常 |
| A（不采用） | 过渡版照旧上传整包 zip，让老客户端正常升级 | 你多上传一次 840 MB（计量网络下未必愿意） |

选项 B 之所以安全：失败发生在 staging 阶段之前，旧版本目录不被触碰；且失败信息仍然指向
"手动安装完整程序包"这条既有出路。已与用户确认：会通知用户下载支持增量更新的新客户端，
因此**不需要**为老客户端保留一次整包上传。

### 5.4 过渡期结论：首个发布必须走整包（2026-09-20 补充）

分片清单只有认识 `Shards` 的客户端能用，而线上 `2026.9.19.2` 是在分片实现**之前**构建的，它不认识
`Shards`。所以直接发分片清单会让所有已安装客户端无法自动升级程序（资源更新不受影响，见 §5.3）。
发分片的那个客户端必须先到达用户手里。两条路线：

| 路线 | 做法 | 用户侧 | 上传量 |
|---|---|---|---|
| **A（推荐）** | 首个发布仍用 `--program-zip` 整包（清单里没有 `Shards`），所有客户端自动升级到"认识分片的客户端"；**下一个发布**才用 `--program-shards true` | 全自动 | 一次完整 zip（≈700 MB） |
| B | 首个发布就用分片，另外再传一个完整 zip 供手动安装 | 用户需手动下载并解压新客户端 | 完整 zip（≈700 MB）**加上** 8 个分片归档（≈704 MB）≈ 1.4 GB |

两条路线都必须上传完整 zip：分片时代的新用户首次安装只能靠它（`Publish-ResourceUpdate.ps1` 的
`-ManualInstallZip`）。既然 A 传得更少、而且对用户是全自动的，**推荐 A**。

这也是对 §13.1 第 8 条的细化：**"不上传整包"只适用于分片时代的常规发布**——那时已安装的客户端都认识
分片，新用户仍靠发行页上那份完整 zip 首次安装。那份 zip 不必每个版本都重传：任何一份**认识分片的**
完整 zip 都能自动增量升级到最新，所以按需刷新即可（脚本在没有 `-ManualInstallZip` 时会明确提示）。

## 6. 客户端流程（阶段 3 已实现）

1. 校验清单（现状不变）。
2. **复用判定**：读**正在运行的那一版的签名清单** `VersionRoot(<当前 id>)/update.json`，逐路径比较其
   `package.Files` 与新版 `Files[]`（`path`+`size`+`sha256` 全同才算可复用）。上一版在本机启动时已被
   `ValidateInstalledAsync` 全量哈希验过，所以**不需要**为了判定再哈希 1.3 GB 本地文件。
   - 例外：`state.Current == ""` 表示"手动安装的原始程序"，此时 `AppDirectory("") == InstallRoot`
     且没有本地签名清单。这种情况每个片都先当作"要下载"，逐一尝试复制并**边复制边算哈希**，
     命中就不下载——远便宜于整包下载（§7.1）。
   - 判定以**片**为单位（7a）：片内任一文件不可复用 → 该片整片下载。
3. **组装 staging**：可复用的文件从上一版本目录**复制**（不移动，保留回滚能力，复制时逐个对
   `Files[]` 校验哈希），要下载的片落到 `staging/<txn>/shards/<id>.zip` 再 `ExtractShardAsync`
   解压（§7.2）；片 zip 用完即删。
4. 逐文件校验 `VerifyDirectoryAsync`（现状不变）→ 原生 preflight（现状不变）→ 原子切换（现状不变）。
   复用导致的本地损坏在**复制时就**被发现并让该片改走下载（§7.1），所以这一步不需要重试逻辑。
5. 失败/取消/校验不过：删 staging，旧版本不受影响（现状不变）。
6. **磁盘空间**：按 `本次要下载的片字节 + Files.Sum + 128 MB` 预留（替换原来的
   `package.Size + package.Files.Sum + 128 MB`）。"要下载"按第 2 步的清单判定算；手动安装那一档因为
   无法先验清单，按"全部片"保守预留。
7. **下载回调改为按目标**：`PrepareAsync` 的回调从 `(ProgramPackage, Stream, ct)` 改成
   `(ProgramDownloadTarget, Stream, ct)`（`Name/Url/Size/Sha256`），整包发布传 `program.zip`，
   分片发布每片传 `<片名>.zip`。调用方（`UpdateService.PrepareProgramAsync`）只搬字节，
   进度文案变成"下载新版程序 &lt;片名&gt;"。

## 7. 两种实现粒度（取舍）

- **7a 分片级复用（决定先做）**：以"片"为最小下载单位，片内任一文件不同则整片重下。
  实现简单、请求数少；因为同一片内的文件变化高度相关（改 UI 就是那几个 dll），日常收益已经接近
  文件级。
- **7b 文件级复用（留后）**：片内再按文件挑选，进一步省几 MB，但需要"按文件定位 zip 内条目"或
  "每片内再分小段"（zip 需支持随机访问 / 用 zip 中央目录按条目取流）。复杂度明显上升，收益边际。

### 7.1 复用必须可自我修复（阶段 3 已实现，做法比原计划更早、更准）

今天每个字节都是新下载的，本地文件损坏会被下载自然修好。引入复用后，一个坏文件会让整次更新
**永远**失败——因为最终 `VerifyDirectoryAsync` 会对 `package.Files` 的每个文件查哈希，失败即整次
准备失败。

原计划是"组装后全量校验失败 → 把该片标为必须下载 → 重跑一次"。实现时改成了**更早、更省**的做法：

- 复制每一个复用文件时就**流式算 SHA-256**（不额外读一遍盘），与 `Files[]` 比对；不符（或本地缺失、
  长度不对、或该路径已被别的片写过）就返回失败。
- 失败时只清理**这个片**刚写下的文件，然后该片正常走下载路径。所以修复粒度是"片"，且不需要任何
  重试循环，也不会因为一个坏文件把整包重下。
- 好处：判定与修复合成一次 I/O；`state.Current == ""`（没有签名清单、只能靠哈希判定）这一档也
  天然被同一条路径覆盖。
- 最终 `VerifyDirectoryAsync` 仍然对整棵树跑一遍（现状不变），它现在只是最后一道防线，不是修复
  机制的触发器。

### 7.2 按片解压：新增方法，但不要复制 zip 安全解析（阶段 2 已实现的共享版本）

`ProgramPackageValidation.ExtractAsync` 不能按片复用：它先 `Validate(package)`（要求
`RequiredFiles` 齐全），还要求 zip 条目与 `package.Files` **恰好一一对应**，片 zip 两条都不满足。
现在的做法（`ProgramPackageValidation.cs`）：

- 新增 `ExtractShardAsync(archive, destination, package, shardId)`：分片必须真的属于这个包（按 `Id`
  取签名清单里的那一条，而不是信调用方传进来的对象），校验片 zip 的 `size`/`sha256`，要求条目集合与
  "该片声明的路径子集"**恰好一一对应**（数量、长度、目录条目、禁止链接/重复/特殊类型），再逐文件
  流式解压、限制单文件长度、逐文件对 `Files[]` 校验。目标目录可以已存在（多个片写同一棵树），
  但同一个路径写第二次会失败而不是被覆盖。
- zip 安全解析抽成两个私有 helper：`ReadArchiveEntries`（打开 zip、拒绝链接/重复/特殊文件、条目与
  期望清单比对）与 `ExtractEntriesAsync`（按 `SafeChild` 落盘、长度上限、逐文件校验），整包与分片
  两条路径都调用它们，不会各写一份。
- 最终仍由既有的 `VerifyDirectoryAsync` 对组装结果做全量校验——这正是"复用安全"的真正保证，
  所以发布器的 `prepare` 就是拿它当阶段 2 的验收回归用的（§9）。

## 8. `assets-*` 分片与资源渠道的关系

三种做法，评审决定 **A1**（理由见下）：

- **A1（决定）程序分片包含 assets-\***：与今天一致（开箱即用），同一份文件清单在"程序包"与
  "资源包"里各出现一次。日常下载不重复花钱——两个渠道都按文件清单差分，未变就沿用旧 URL。
- **A2 程序分片只覆盖程序本体**（`runtime`/`core`/`ui`/`assets-misc`），
  `Assets/KuroMap`、`KuroMapIcons`、`KuroTilePacks` 交给资源渠道，首次安装由安装程序或首次运行时
  的"预置资源"提供。
- **A3 把区域包整体移出程序包**（方案 B）：程序包 ≈200 MB，代价是首次启动必须下载。

**为什么选 A1 而不是原方案推荐的 A2：**

1. 本仓库**没有安装程序**（发行物就是 `New-ProgramReleasePackage.ps1` 打出的 zip），A2 说的
   "安装程序或首次运行预置资源"目前没有承载者。改 A2 会让新用户先下程序、再下 587 MB 才能看到
   地图，是**功能回退**。
2. 更硬的理由：`ProgramUpdateStore.CheckNativeAsync`（`ProgramUpdateStore.cs:236-263`）拿
   `Assets/Updates/bundled-snapshot.json` 去验**组装后的程序目录**。A2 下全新安装的目录里没有
   `KuroMap`/`KuroMapIcons`/`KuroTilePacks`，preflight 很可能直接失败，把"安装新版程序"这条唯一
   通道卡死。A1 让这条安全链与今天完全一致。
3. A1 的"重复"只是发布侧的一次性上传（未变则沿用旧 URL），日常更新行为与 A2 等同（都只下变化片）；
   A2 与 A3 的差别本来就只是"首次安装体积"。

**后续可做（不在 v1）——跨渠道引用**：若某 `assets-*` 片的文件列表与某个资源包完全一致，就让
`ProgramShard.Url/Size/Sha256` 直接沿用那个资源包的 URL。**纯发布侧改动，客户端不用改**，且仍然
满足"签名清单点名每一份上传字节"。它能解决"瓦片在资源渠道改过、下一次程序发版又被重新打片并上传
536 MB"的问题，也就是 A2 的带宽收益 + A1 的行为。

## 9. 发布侧改动

分片发布用 `--program-release true --program-shards true` 打开；不传 `--program-shards` 时
`--program-zip` 的整包路径保持原样（老流程不受影响）。

- `UpdatePublisher prepare`（阶段 2 已实现）：
  - `ClassifyProgramRoot` 把 `--app-root`（就是 `New-ProgramReleasePackage.ps1` 里先跑
    `Test-ProgramReleasePackage.ps1` 再打包的那个暂存目录）读成权威清单 `Files[]`，并按 §4 的
    `ShardMap` 规则分组：未识别路径、缺必需文件、空片都直接失败；
  - `PackShard` 每片打一个 zip，规则与资源包打包器一致（`ZipEpoch` + 条目已按路径排序 +
    `CompressionLevel.Optimal`），所以同一份内容永远得到同一串字节。**不要**复用
    `New-ProgramReleasePackage.ps1:117` 的 `ZipFile::CreateFromDirectory`——它保留文件时间戳与枚举
    顺序，会打出不同字节；
  - 差分复用：片内文件清单（`path`+`size`+`sha256`，用 `FileListsEqual` 与上一版签名包比对）完全不变
    时，仍会在本地重新打包一次并**断言字节与上一版相同**，然后沿用上一版的 `Id/Url/Size/Sha256`
    （连资产名一起保留，所以旧 URL 永远指向真实存在的文件）；
  - **整包字段指向分片描述符**（§5.3 选项 B）：`prepare` 写出
    `program/IMao-v<版本>-shards.json`（片清单 + 总量），`ProgramPackage.Url/Size/Sha256` 指向它；
    `Files[]` 仍是完整权威清单。这样老客户端会下几 KB 然后拒绝该版本，而不是开始 840 MB 下载；
  - **验收回归**：`prepare` 用 `ExtractShardAsync` 把所有片（含沿用的旧片）解压回
    `program-reassembly/`，再交给**原封不动的** `VerifyDirectoryAsync` 验收——验收标准不变，只是
    组装方式变了。不通过就不签名；
  - `SelfTest` 覆盖：片是 `Files[]` 的完整划分、指针指向本 tag 的描述符、每个片 zip 与签名身份一致、
    重组结果通过校验、第二次 prepare 全片沿用旧 URL（`fixture-3`）、只改一个片时**只有该片与
    `ui` 片**（因为 `build-info.json` 在 `ui`）移到新 tag、同版本改内容被拒。
- `release-report.json`（阶段 2 已实现）：新增 `program` 块（指针 URL/大小/哈希 + 每个片的
  name/url/size/sha256/files），供发布脚本逐片绑定与校验；`assets[]`（资源包）不变。
- `UpdatePublisher verify`（阶段 2 已实现）：当签名程序包有分片且本次输出目录里带 `program/` 时，
  逐片校验 zip 与描述符；资源-only 输出会明确报告"程序包是沿用的"。
- `Publish-ResourceUpdate.ps1`（阶段 4 已实现）：整包路径仍是 `-ProgramZip` + 同名 `.report.json` 与
  `catalog.app.package` 绑定。分片路径改成：
  - 新增 `scripts/ResourceUpdateAssets.ps1`（可单测的纯函数，与 `ResourceUpdateCatalog.ps1` 同风格）：
    `Get-ProgramShardAssets` 对签名清单里的**每一个片与描述符**核对本地归档的存在、长度与 SHA-256，
    然后按 URL 分流——指向本次 tag 的进上传列表，指向历史 tag 的（未变片）留在原地并进入可达性检查；
    `Get-RetainedProgramAssets` 处理"本次只是把已发布程序带过去"的资源-only 发布。
  - `release-report.json` 新增 **`programPrepared`** 正向信号：只有本次真的构建了程序（`--program-release`
    且产出成功）才会上传程序归档；否则只核对已发布 URL 的可达性。**不用"目录存不存在"之类的猜测**。
  - 沿用原有的逐附件上传前校验（`:56-59`）、远程 digest 复核（`Assert-RemoteAssetBytes`）、单附件
    2 GiB 上限与最后的公开 HEAD 可达性检查（保留的旧 URL 也在检查范围内）。
  - `-ProgramZip` 与分片发布互斥（会直接拒绝），避免两种绑定混用。
  这样"签名清单点名每一份上传字节"这条不变量在分片发布下依然成立。
- `ProgramPackageValidation`：程序包验收（`Test-ProgramReleasePackage.ps1`）继续对**组装后的目录**
  做校验：把"分片组装结果"当作今天的 `app` 目录来验（验收标准不变，只是组装方式变了）。

## 10. 收益（阶段 2 真实干跑实测）

数字都是**压缩后**的下载量（用户实际付的流量），取自 §4.2 的真实干跑。

| 更新类型 | 整包 | 分片后（7a） | 节省 |
|---|---|---|---|
| 只改 UI/桥（最常见） | 840 MB | **`ui` 29.8 MB**（启动器带版本号，每次必变，见下） | ≈96% |
| 改 CoreHost | 840 MB | `ui` 29.8 + `core` 25.7 = 55.5 MB | ≈93% |
| 改地图点位/攻略 | 840 MB | `assets-map-data` **1.6 MB** | ≈99.8% |
| 改图标包 | 840 MB | `assets-map-icons` 33.6 MB | ≈96% |
| 换 .NET 运行时或 Paddle | 840 MB | `runtime` 142.2 MB | ≈83% |
| 只改瓦片包 | 840 MB | `assets-tiles` 437.0 MB | ≈48% |
| 首次安装 | 840 MB | A1：全部片合计 **703.9 MB** | 略小 |

**口径说明（必须看清楚）**：840 MB 是当前线上 `2026.9.19.1` 的整包，那一版还包含已退役的基础库
（150.3 MB）；同口径的 `.19.2` 整包预计与分片合计相当（703.9 MB）。所以"首次安装"一行几乎是平的，
真正的收益在**日常更新**：最常见的改动从下 840 MB 变成下 30 MB 级。

**实测发现（阶段 1–2）**：

1. `ui` 片解压 **69.0 MB**、压缩后 **29.8 MB**，其中 `IMao-Launcher.exe` 独占 67.7 MB 解压。
   该 exe 带 `FileVersion = appVersion`（`New-ProgramReleasePackage.ps1:40` 强制），所以它的字节
   **每个发版都变** → `ui` 片每次发版必重下。**"只改 UI"的真实下限是 ≈30 MB（压缩后），不是原估算的
   12 MB**；要把常见更新压到 10 MB 级，只能让启动器版本资源与发版解耦（例如启动器只作为稳定引导器、
   版本号另存描述符），这属于打包约定变更，不在本方案范围。
2. `core` 只有 `IMao-CoreHost.exe` + `common.dll`，且该 exe **没有版本资源**（实测
   `FileVersion`/`ProductVersion` 为空），所以它只在真正改 CoreHost 时才变——"改 CoreHost 经常发生"
   的原假设不成立，`core` 是低频片。
3. 压缩比差别很大：`assets-map-data` 16.4 MB → **1.6 MB**、`runtime` 442 MB → 142 MB，
   而 `assets-tiles` 536 MB → 437 MB（瓦片本身是二进制特征，压不动）。所以"只更新点位/攻略"是最爽的
   场景，瓦片变化则仍然贵——但瓦片变化本来就走资源渠道按区域包分发（§3），不必随程序发版。
4. `runtime` 仍是唯一的大尖峰（142 MB）；要收窄它见 §4.1 第 5 条（拆
   `runtime-dotnet`/`runtime-native`）。首次安装一行按 A1 计算；A2/A3 的差别在首装体积，
   日常更新收益三者相同。

## 11. 风险与不做的事

- **不就地增量覆盖**：仍然 staging + 原子切换，避免半更新状态。**分片只影响"字节怎么到本机"；
  `Files[]` + `VerifyDirectoryAsync` + 原生 preflight + 原子切换这条链一个都不动（评审新增）。**
- **复用引入的损坏风险由 §7.1 的"边复制边校验、坏片改走下载"兜住（已实现）**，否则复用会带来
  "本地坏一个文件就再也更新不了"的新失败模式。
- **不引入对象存储/CDN**：继续用 GitHub Release 附件（片数个位数）。
- **不改签名算法**：沿用 P-256/SHA-256 与 `update.json` 封装。
- **不改启动器协议**：启动器只负责"退出后切换目录"，与分片无关。
- **分片边界一旦固定**：后续调整边界会让相关片重下一次（可接受）。
- **回滚**：保留上一版本目录（现状），分片不改变回滚语义。
- **失败可重试**：单片下载失败时整次准备失败，旧版本不动；重试从零（可后续加分片级断点）。
- **不改老客户端的资源更新**：见 §5.3，分片只影响老客户端的程序更新路径。
- **离线集合包仍只装资源包**：`resources-<版本>-offline.zip` 服务的是地图资源的离线安装，程序分片
  仍从发行附件取（离线导入本来也要求"先升级程序"，见 `UpdateService.LoadOfflineAsync`）。

## 12. 分阶段实现与每阶段验证（评审调整顺序）

**阶段 1–4 已完成（2026-09-20）**，阶段 5–6 未开始。

1. ✅ **契约 + 分片映射表 + 校验**
   - `UpdateContracts.cs`：`ProgramPackage.Shards` 与 `ProgramShard`（`Id/Url/Size/Sha256/Files`，
     `Files` 只存路径）。
   - `ProgramPackageValidation.Validate` → `ValidateShards`：空 `Shards` 保持老整包路径合法；
     非空则要求片标识唯一合法、片元数据合法（本仓库下载地址、`0 < Size < 2 GiB`、合法哈希）、
     片内文件非空、每个路径都在 `Files[]` 里且只属于一个片、片数 ≤ 16、并集恰好覆盖 `Files[]`。
   - `tools/UpdatePublisher/ShardMap.cs`：§4 那张表的唯一一份实现（有序规则 + 未识别返回 null）。
   - `tools/UpdatePublisher` 新增 `shard-map --app-root <dir> [--report <json>]`：在真实暂存根上跑
     分类，未识别即失败，并断言每个片非空、`RequiredFiles` 全部存在。
   - 验证：`scripts/Test-ProgramUpdates.ps1` **31 项通过**（新增 3 项分片用例：合法划分被接受、
     9 类非法划分被拒、签名 JSON 往返保留分片）；发布器 `self-test` **24 项通过**（新增分片表断言、
     fixture 根分类、未识别文件被拒）；`shard-map` 在真实 2026.9.19.2 暂存根上 **1222 文件零未识别**。
2. ✅ **发布器**：`prepare --program-release true --program-shards true` 从 `--app-root` 产出片
   （`ClassifyProgramRoot` + 确定性 `PackShard`）+ 差分复用（沿用旧 URL，并断言重打包字节一致）+
   整包字段指向分片描述符 + `release-report.json` 的 `program` 块 + `verify` 逐片校验。
   **同时落了最关键的那条回归**：`prepare` 把所有片（含沿用的旧片）解压重组到
   `program-reassembly/`，再交给**原封不动的** `VerifyDirectoryAsync` 验收——验收标准不变，只是
   组装方式变了；不通过就不签名。
   验证：`self-test` **28 项通过**（新增 4 项：完整划分与描述符绑定、全片沿用旧 URL、只改一片时只有
   该片与 `ui` 移动、同版本改内容被拒）；真实 `app-root` 干跑通过（测试密钥、不发布）：
   **7 片 703.9 MB**，重组 **1222 文件 / 1179.4 MB** 与暂存树一致，`verify` 报 7 片 + 描述符 + 15 个
   资源包全部通过。证据：`out/shard-phase2/real-prepare/`。
3. ✅ **客户端**：`PrepareAsync` 按片下载（回调改成 `ProgramDownloadTarget`）+ 读正在运行那一版的
   签名清单做片级复用 + **边复制边校验、坏片改走下载** + 磁盘空间按需。整包发布路径保持原样
   （回调收到 `program.zip`）。
   验证：`scripts/Test-ProgramUpdates.ps1` **35 项通过**，新增 4 项分片用例——
   ①全新安装每片只下一次并组装出通过校验的程序；
   ②第二版只下"变了的片 + `ui` 片"（`core`/`runtime` 从正在运行的目录复制）；
   ③手动破坏一个本可复用的文件后，**只有该片**重新下载，更新成功且文件内容正确；
   ④手动安装（无签名清单）时靠边复制边哈希证明复用，**0 字节下载**。
   `VerifyDirectoryAsync`/preflight/原子切换路径未改动。
   另外做了**真实分片集的端到端本地验证**（`Tests/ProgramUpdates` 的 `real-program` 入口，对
   `out/shard-phase2/real-prepare` 跑）：全新安装取全部 7 片、组装 1222 文件 / 1179.4 MB 并通过校验与
   提交（30.3 s）；把同一棵树当作手动安装（无签名清单）再跑一次，**0 下载**、1222 个文件全部靠
   "边复制边哈希"复用并通过校验（24.4 s）。两次得到同一个版本目录标识
   `2026.9.19.2-b4c5c6fa39bff78c`。证据：`out/shard-phase3/real-client/`。
4. ✅ **上传绑定**：`Publish-ResourceUpdate.ps1` 通过新 helper `scripts/ResourceUpdateAssets.ps1` 逐片
   绑定后上传；`-ProgramZip` 与分片发布互斥；保留 URL 的旧片进可达性检查；`release-report.json` 增加
   `programPrepared` 正向信号（只有本次构建了程序才上传程序归档）。
   验证：新增 `scripts/Test-ProgramShardReleaseAssets.ps1`（**8 项通过**：本 tag 的片与描述符进上传、
   未变片保留旧 URL、资源-only 发布核对全部已发布 URL、本地字节/长度/缺失/描述符不符/无分片清单均被
   拒），并已挂到 `scripts/Test-Runtime.ps1`；对真实 prepared 输出跑同一函数得到**上传清单预览**
   （7 片 + 描述符全部绑定、0 个保留）。
5. **端到端**：一次真实程序更新（本地 + 线上各一次），记录实际下载字节数与旧值对比。
   **需要用户执行**（本机无法代做：要提交改动、用该提交重跑原生+WinUI 候选构建、再用生产私钥发布）；
   逐条命令、前置检查与发布后验证已写进 `Docs/ResourceUpdates.md` 的「过渡版与分片版的发布差异」。
6. **收尾**：文档（`Docs/ProgramUpdates.md` 已在阶段 3/4 更新）+ 一次正式发布（见上）。

## 13. 评审结论与待决项

### 13.1 已定决策（2026-09-20 评审）

阶段 1（契约 + 分片表 + 校验）、阶段 2（发布器产出分片）、阶段 3（客户端按片下载与复用）与阶段 4
（逐片上传绑定）已按下列决策实现并通过测试，见 §12。

1. **分片表按 §4 切**（实测值），并加 §4.1 的边界硬规则（每个发版都变的文件钉在 `ui`、启动器放
   `ui`、有序路径规则 + 未识别即报错、客户端只校验结构不认片名）。
2. **`assets-*` 采用 A1**（程序分片照旧携带资产），跨渠道引用与 `runtime` 拆分留作后续。
3. **先只做 7a**（分片级复用），7b（文件级）留后。
4. **复用判定走上一版签名清单** `VersionRoot(<当前 id>)/update.json`，不为复用哈希 1.3 GB 本地文件；
   `state.Current == ""` 时退回按新清单逐文件哈希。
5. **复用必须带"验证失败就从网络补下该片一次"的修复路径**（§7.1）。
6. **分片清单里只放路径**，`size`/`sha256` 以 `Files[]` 为准（§5.2）。
7. **片 zip 必须字节可复现**，且**每一片都要用签名清单绑定后上传**，保住"签名清单点名每一份上传
   字节"（§9）。
8. **老客户端兼容按 §5.3 的选项 B 处理**：分片时代的常规发布不再上传整包 zip，`Url/Size/Sha256`
   指向真实小文件让老客户端快速失败；资源更新不受影响。**用户 2026-09-20 确认**：会通知用户下载
   支持增量更新的新客户端。**§5.4 补充**：首个发布改走整包路径（推荐 A），让所有客户端自动升级到
   认识分片的客户端；每个版本仍可用 `-ManualInstallZip` 附一份完整 zip 供新用户首次安装。

### 13.2 仍未决（后续排期）

1. **`runtime` 何时拆分**（§4.1 第 5 条）？建议等下一次 .NET 或 Paddle 升级前决定。
2. **跨渠道引用是否要做**（§8 末）？若上传带宽吃紧就优先做，纯发布侧改动。
3. **分片级断点续传**是否要列入下一轮（§11 目前是"失败重试从零"）。
