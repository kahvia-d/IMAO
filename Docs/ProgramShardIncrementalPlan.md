# 程序本体分片增量更新方案（待评审）

> 状态：**方案，未实现**。用户 2026-09-20 要求先看方案再决定实现。
> 相关代码：`IMao-WinUI.Core/Updates/ProgramUpdateStore.cs`、`ProgramPackageValidation.cs`、
> `UpdateContracts.cs`、`tools/UpdatePublisher/Program.cs`、`scripts/Publish-ResourceUpdate.ps1`。

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

分片按"**变化频率**"切，不按目录切；片数控制在个位数，避免附件与请求数膨胀：

| 分片 | 内容 | 当前大小 | 变化频率 |
|---|---|---|---|
| `runtime` | 自包含 .NET 运行时、`paddle_inference.dll`、`mklml/mkldnn/libiomp5md`、`opencv_world*.dll`、`vcomp140`、`Microsoft.*` 等 | ≈500 MB | 换 .NET/依赖时 |
| `core` | `IMao-CoreHost.exe` + 其直接依赖 | ≈35 MB | 经常 |
| `ui` | `IMao-WinUI.exe`、`IMao-WinUI.dll`、`IMao-WinUI.Core.dll`、`KuroSyncBridge.exe`、`resources.pri`、清单/许可/配置 | ≈12 MB | 最常 |
| `assets-misc` | `Assets/models`、`Assets/其他`（不含 `KuroMap`/`KuroMapIcons`/`KuroTilePacks`） | ≈45 MB | 偶尔 |
| `assets-map-data` | `Assets/KuroMap`（点位/状态/攻略/校准/准入） | 16.3 MB | 偶尔 |
| `assets-map-icons` | `Assets/KuroMapIcons` | 34.9 MB | 偶尔 |
| `assets-tiles` | `Assets/FeaturesDatas/KuroTilePacks/*` | 535.9 MB | 基本不变 |

`assets-*` 三片与资源渠道的包**同源**：同一份文件清单已经在 `ResourcePackage.Files` 里。
是否让程序分片直接**引用**资源包（而不是再打一份），见 §8 的取舍。

## 5. 清单格式（向后兼容）

在 `ProgramPackage` 上新增分片数组，**保留**现有 `Url/Size/Sha256/Files` 作为整包回退：

```csharp
public sealed record ProgramShard
{
    public string Id { get; init; } = "";                     // runtime / core / ui / assets-*
    public string Url { get; init; } = "";
    public long Size { get; init; }                            // zip 字节数
    public string Sha256 { get; init; } = "";
    public List<ResourceFile> Files { get; init; } = new();     // 片内文件：path/size/sha256
}

public ProgramPackage { ... 现有字段不变 ...; public List<ProgramShard> Shards { get; init; } = new(); }
```

- 新客户端：`Shards` 非空则走分片路径；为空则按老路径下整包。
- 旧客户端（不认识 `Shards`）：`UpdateSignature.ValidateCatalog` 会**忽略未知字段**吗？
  当前校验只读它认识的字段，未知字段不会导致失败（`System.Text.Json` 默认忽略）。
  但**旧客户端的 `ValidateCatalog` 会不会因为 `Files`/`Url` 变化而拒绝**？不会——整包字段照旧填。
  所以**同一份清单可以同时服务新老客户端**，一次发布即可。
- 分片清单必须是 `Files[]` 的**完整划分**（不重不漏），发布器与客户端都断言这一点。

## 6. 客户端流程（保持现有安全属性）

1. 校验清单（现状不变）。
2. **先算要下什么**：对每个分片，逐文件在本机**当前版本目录**（`VersionRoot(<当前 id>)/app`）里找同名文件，
   比对 `sha256` 与 `size`：
   - 全部命中 → **跳过该分片**（0 字节下载）；
   - 部分命中 → 只取缺失/不同的文件（见 §7 的实现取舍）；
   - 全部未命中 → 整片下载。
3. **组装 staging**：命中文件从当前版本目录**复制**（不移动，保留回滚能力），未命中文件从分片 zip 解压。
4. 逐文件校验 `VerifyDirectoryAsync`（现状不变）→ 原生 preflight（现状不变）→ 原子切换（现状不变）。
5. 失败/取消/校验不过：删 staging，旧版本不受影响（现状不变）。
6. **磁盘空间**：按"实际需要下载的字节 + 组装峰值"预留，而不是整包
   （现状按 `package.Size + Files.Sum` 预留，分片后要改）。

## 7. 两种实现粒度（取舍）

- **7a 分片级复用（推荐先做）**：以"片"为最小下载单位，片内任一文件不同则整片重下。
  实现简单、请求数少；因为同一片内的文件变化高度相关（改 UI 就是那几个 dll），
  日常收益已经接近文件级。**建议第一步只做这个。**
- **7b 文件级复用**：片内再按文件挑选，进一步省几 MB，但需要"按文件定位 zip 内条目"或
  "每片内再分小段"（zip 需支持随机访问 / 用 zip 中央目录按条目取流）。
  复杂度明显上升，收益边际。留作后续。

## 8. `assets-*` 分片与资源渠道的关系

三种做法，建议 A2：

- **A1 程序分片包含 assets-\***：与今天一致（开箱即用），但同一份字节在"程序包"与
  "资源包"两套清单里各出现一次，更新路径重叠、磁盘上只有一份（程序目录）但语义重复。
- **A2（建议）程序分片只覆盖程序本体**（`runtime`/`core`/`ui`/`assets-misc`），
  `Assets/KuroMap`、`KuroMapIcons`、`KuroTilePacks` 交给**资源渠道**（它们已经是可选/可删/差分的包）。
  首次安装由安装程序或首次运行时的"预置资源"提供（等价于今天程序包里的那份，只是一次性）。
  这样两套机制职责清晰，也不会重复携带 587 MB。
- **A3 把区域包整体移出程序包**（方案 B）：程序包 ≈200 MB，代价是首次启动必须下载。

A2 与 A3 的差别是"首次安装体积"；**日常更新收益三者相同**（都只下变化片）。

## 9. 发布侧改动

- `UpdatePublisher prepare`：
  - 按 §4 的分片表切分 `app-root`，每片打一个 zip，产出 `Shards[]`；
  - 沿用现有"内容不变则保留旧包的版本与 URL"的差分逻辑（`FileListsEqual(prior.Files, built.Files)`
    那段）：`runtime`/`assets-*` 通常整片不变 → **不重传**；
  - 自测新增断言：①分片是 `Files[]` 的完整划分；②第二次 prepare 时不变片沿用旧 URL；
    ③分片 `Url` 指向本次 tag 或历史 tag（都在允许范围内）。
- `Publish-ResourceUpdate.ps1`：除 `update.json`/离线包外，上传**缺失的分片**；
  附件数从 1 增加到 ~5，远低于任何限制；`Assert-RemoteAssetBytes` 逻辑照可用于每片。
- `ProgramPackageValidation`：程序包验收（`Test-ProgramReleasePackage.ps1`）继续对**组装后的目录**
  做校验：把"分片组装结果"当作今天的 `app` 目录来验（这是关键——验收标准不变，只是组装方式变了）。

## 10. 收益估算（基于 §2 的真实数字）

| 更新类型 | 今天 | 分片后（7a） | 节省 |
|---|---|---|---|
| 只改 UI/桥（最常见） | 840 MB | `ui` ≈12 MB（实际变化几 MB） | ≈98% |
| 改 CoreHost | 840 MB | `core`+`ui` ≈47 MB | ≈94% |
| 改地图点位/攻略 | 840 MB | `assets-map-data` 16 MB | ≈98% |
| 换 .NET 运行时或 Paddle | 840 MB | `runtime` ≈500 MB | ≈40% |
| 首次安装 | 840 MB | 840 MB（不变） | 0 |

（归档基础库后新程序包预计 ~700 MB，上表按当前 840 MB 计。）

## 11. 风险与不做的事

- **不就地增量覆盖**：仍然 staging + 原子切换，避免半更新状态。
- **不引入对象存储/CDN**：继续用 GitHub Release 附件（片数个位数）。
- **不改签名算法**：沿用 P-256/SHA-256 与 `update.json` 封装。
- **不改启动器协议**：启动器只负责"退出后切换目录"，与分片无关。
- **分片边界一旦固定**：后续调整边界会让相关片重下一次（可接受）。
- **回滚**：保留上一版本目录（现状），分片不改变回滚语义。
- **失败可重试**：单片下载失败时整次准备失败，旧版本不动；重试从零（可后续加分片级断点）。

## 12. 分阶段实现与每阶段验证

1. **契约 + 校验**：`ProgramShard` 与 `ValidateCatalog` 完整性断言（分片是 `Files[]` 的完整划分）。
   验证：托管 `Tests/ResourceUpdates` 新增用例 + 发布器自测。
2. **发布器**：`prepare` 产出分片 + 差分复用 + 上传缺失片。
   验证：自测断言"第二次 prepare 不变片沿用旧 URL"；对真实 `app-root` 跑一次 prepare（不发布）。
3. **客户端**：`PrepareAsync` 分片下载 + 本地复用 + 磁盘空间按需。
   验证：托管用例用假网络断言"只请求变化片"；`VerifyDirectoryAsync`/preflight/原子切换路径不变。
4. **端到端**：一次真实程序更新（本地 + 线上各一次），记录实际下载字节数与旧值对比。
5. **收尾**：文档（`Docs/ProgramUpdates.md`）+ 一次正式发布（与 2026.9.19.2 一起或之后）。

## 13. 待用户拍板

- 分片表是否按 §4 切（片名/粒度）？
- `assets-*` 采用 A1 还是 A2？（A2 让程序包与资源渠道职责清晰，但需要一次性预置资源）
- 是否先只做 7a（分片级复用），把 7b（文件级）留后？
