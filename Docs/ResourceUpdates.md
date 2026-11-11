# 程序与地图资源更新

生产发布请使用 [CloudRelease.md](CloudRelease.md) 中的云端构建、本地 DPAPI 双签名流程。`publish` 默认关闭，原 `prepare` 保留用于兼容和本地验证；正式发布必须带已登记请求与签名响应，不再使用下文历史本地直接上传步骤。


使用者首次手动安装带更新功能的完整程序包。2026.9.9.4 起包含独立启动器，程序与资源均可在设置的“版本与地图资源”中下载。新版程序下载并校验完成后，可以继续使用，也可以点击“退出并更新”；正常退出后再打开也会启用。不自动下载、不强制重启。旧清单未提供程序包签名信息，或新版要求更高的启动器协议时，按钮仍打开正式发行页。离线资源使用“导入离线包”选择 `resources-四段版本-offline.zip`。回退在下次启动生效。程序自更新布局与故障恢复详见 [ProgramUpdates.md](ProgramUpdates.md)。

更新不会包含或覆盖完成记录、路线、筛选选择与个人设置。首次启用没有确认成功时，下次启动恢复上一成功快照。错误签名、损坏资源、下载失败或磁盘不足均保留当前资源。

已安装资源放在 `%LOCALAPPDATA%\IMao-WinUI\ResourceUpdates` 的版本目录。首版保留历史已安装包，不自动清理这些目录——`snapshots\v2\<快照标识>.json` 是这条承诺的落点：它保留该次发布的全部签名包记录，所以玩家以后删掉某个地区再想启用，不必重新下载整套；`packages\<包 ID>` 同理按包保留。下载与安装的临时文件由事务结束时清理，并在**每次启动时**再扫一遍 `staging` —— 事务的清理写在 `finally` 里，而进程被关闭、被启动器的进程组结束、或断电时那个 `finally` 不会执行，只靠它会让一次中断永久留下一份下载好的包（2026-10-10 实测过一个 69.6 MB 的残留，日期停在 2026-09-19，两个包其实都已经解压并安装到位）。启动时的清扫持有与事务相同的锁，因此扫到的一定是遗留物；`staging` 本身是符号链接/目录联接时整块不动。**同一时刻还会清掉 `snapshots` 下没人引用的描述文件缓存**（`bundled-<哈希>.json`、`runtime-<哈希>.json`）——它们是按内容命名的缓存，缺了下次启动自动重建，与上面那条"保留已安装包"的承诺无关（实测一个安装积了 218 个、只被引用 1 个）；`v2\` 目录不在此列，永不清理。不要手动删除运行中、待启用或回退所需的包。

## 资源与版本约定

- `Version.props` 是程序版本与基础资源 ID 的唯一来源，版本为 `2026.9.9.1` 形式的四段数字；基础 IMF/IMX 等规则变化时提高基础资源 ID，并发布程序版本。
- `build-info.json` 在构建输出生成，记录实际 Git 提交、程序版本、基础资源 ID、`sourceDirty` 及 `sourceTreeSha256`（提交 SHA 加本地变化文件的内容摘要）。正式发布必须从已提交的明确源码构建；尚未提交的工作区构建可制作正式密钥签名的本地验收包，但远端发布脚本会拒绝，不把它误报为该提交的正式发行。
- 基础资源继续随程序提供。一个 `map-data` 包完整包含八个现有场景的点位、分类、图标、校准、场景准入和本地攻略数据；`tile` 和 `candidate` 分别按地区打包。五份原先嵌入 DLL 的点位在构建时复制到输出的 `Assets/KuroMap/runtime`，只统一既有两个分类 ID 别名，不改变点位 ID、坐标或状态 ID。
- 构建生成 `Assets/Updates/bundled-snapshot.json`，启用 `map-data`、`kuro-tile-packs.json` 中注册的每个瓦片包，以及 `candidate-packs.json` 中注册的候选包。一个包只有在 `referenceVerification.passed` 为真时才会进入快照；目录存在本身不足以启用它。`requiresGameValidation` 为真的场景还必须在 `Assets/KuroMap/scene-validation.json` 中获得批准：客户端始终传 `--resource-snapshot`，因此 `RuntimeFeatureRepository` 会因任何一个已注册包未获批准而中止整个资源加载，表现为“启动核心失败”。打包还需通过 CoreHost 资源预检。
- 内置包的版本必须与线上清单使用同一内容身份：某包内容与上一份 `updates/stable.json` 中同 ID 的包逐文件一致时沿用清单里的版本，内容有变化时才使用本程序版本（暂存入口默认读取仓库的 `updates/stable.json`，可用 `-PreviousCatalog` 覆盖）。版本一致时客户端会把随程序内置的资源识别为已安装，不重复下载；不一致会让客户端认为这些包缺失并重新下载整套资源。客户端同时要求发布内容全部内置时才不提示资源更新。
- 客户端读取签名清单时按顺序尝试多个来源：先是 `https://raw.githubusercontent.com/kahvia-d/IMAO/main/updates/stable.json`，然后是 `https://gitee.com/tan-xuedong/imao-updates/raw/main/stable.json`。只在国内无代理时不可达的那一跳上换源；**只有"够不到"（连接失败、超时、拒绝服务）才换下一个来源**，而重定向到不受信任主机、响应过大、验签失败一律就地失败，不会静默改用镜像——那是指纹/篡改信号，镜像只负责可达性。每一份来源都必须通过同一个固定公钥验签，所以镜像不引入任何新的信任。最后一个来源的失败原样抛出，只配置一个来源时行为与从前完全一致。程序版本、清单序号、资源快照版本和各包版本分别处理。单独发布资源时保留上一清单的程序信息。**（2026-10-02 起：这一段描述的是「下载源管理」选 GitHub 时的行为；选 Mirror酱 时"有没有新版程序"由 Mirror酱 的 API 回答，清单仍然要读，但只用于逐文件校验与地图资源，详见 [MirrorChyanIntegration.md](architecture/MirrorChyanIntegration.md) §12。）**
- **地址校验以签名为准，不写仓库名**（`UpdateSignature.ValidateUrl` / `UpdatePublisher.RequireGithub`）：只要求 `https`、默认端口、无 userinfo、主机属于 `github.com` 与 GitHub 的签名附件主机，且 GitHub 路径落在 `/releases/` 内。**载荷的可信度完全来自固定 P-256 公钥对清单的签名，以及清单内记录的每个文件 SHA-256**；URL 规则只是传输卫生，**不是安全边界**。因此**不要**再把 `owner/repo` 写回这条规则。

  > ⚠️ **2026-09-24 的真实故障（务必记住）**：仓库改名 `kahvia-d/WWMAP-TOOLS` → `kahvia-d/IMAO` 之后，旧客户端**无法再更新任何东西**。
  > 原因是当时的白名单要求路径以 `/kahvia-d/WWMAP-TOOLS/releases/` 开头，而 GitHub 对旧路径返回
  > `301 → github.com/kahvia-d/IMAO/releases/...`，**重定向目标被客户端拒收**：
  > ```
  > github.com/kahvia-d/WWMAP-TOOLS/releases/download/<tag>/<asset>
  >   301 → github.com/kahvia-d/IMAO/releases/download/<tag>/<asset>   ← 被旧客户端拒绝
  >   302 → release-assets.githubusercontent.com/...
  > ```
  > 清单本身**不重定向**（`raw.githubusercontent.com` 用旧名仍返回 200），所以界面能显示新版本说明，
  > **一到下载就失败**，报「更新地址必须来自本项目的 GitHub Releases」。这是**自锁**：修复只能靠一次更新发出，而那次更新正是发不出去的那次。
  >
  > **教训**：任何把"仓库名 / 域名以外的固定路径"写进安全校验的设计，都会把一次改名变成一次停服。
  > 需要"只认自己"的地方，应该用**密钥签名**这种**与部署位置无关**的身份，而不是 URL。

- 客户端按发布密钥分别记录该渠道已验证过的最高清单序号与内容哈希，本地测试密钥或预览渠道不会污染正式渠道。只有清单所属程序版本早于当前运行版本时（即回放旧清单的场景）序号下降才会被拒绝；同一程序线内的序号下降或同序号不同内容会在记录中重新同步，并在界面与更新日志里说明。旧版客户端写入的单一全局序号由下一次检查时首个验证通过的渠道继承，不会被丢弃。确实需要重建记录时，可在设置中使用“修复更新状态”，该操作仍会用内置公钥验证当前线上清单。
- 私钥保存在仓库外，以 Windows 当前用户 DPAPI 加密；客户端只携带 `Assets/Updates/trusted-keys.json`。清单使用 P-256/SHA-256、P1363 签名，封装字段为 `keyId`、Base64 `payload`、Base64 `signature`。私钥换机须先规划密钥迁移与客户端公钥升级；不要删除旧密钥后重新初始化同名密钥。

## 维护者本地环境与签名边界

任何 build / test / publish 前先执行 `. ./scripts/Enter-DevEnvironment.ps1`，使用它提供的便携 SDK 与仓库 NuGet 缓存。生产签名材料只在受保护的本机配置中读取，不能写入文档、日志、提交或 CI。

已有发布身份不得因当前进程找不到本机配置而重新生成。Windows 打包应用的目录重定向、执行账户不同及 DPAPI 用户绑定都可能导致访问失败；应在原配置账户中诊断并显式报错，不扫描和输出敏感位置。首次身份建立与密钥轮换需单独审核；日常发布使用 [CloudRelease.md](CloudRelease.md) 的小请求双签名助手。

## 构建、验包与离线包

本地独立候选验收可使用 `scripts/Build-ReleaseCandidate.ps1`（生产构建入口是 Cloud release build），要求当前工作区干净且 `SourceCommit` 等于当前 HEAD。脚本读取已经验证的 VS 2022 原生配置，在新的 CMake 目录中重新编译 CoreHost 和原生测试，重建 WinUI 后生成自包含发布，并输出独立程序 ZIP。每个阶段检查源码未变化；原生构建收据记录提交、版本与实际 CoreHost SHA-256，正式打包必须核对收据，不能拿旧 EXE 搭配新清单。

```powershell
& scripts/Build-ReleaseCandidate.ps1 -OutputRoot out/release-candidate-2026.9.9.4 `
  -SourceCommit (git rev-parse HEAD) `
  -NativeConfigurationDirectory out/build/windows-x64-release-vs144
```

输出包括 `native-build`、`native/native-build-info.json`、托管构建日志、`publish` 和 `program`。该命令不提交、不推送、不创建线上发行。输出目录必须尚不存在，失败记录会保留。首次配置其他机器时先建立经过验证的 VS 2022/x64 CMake 配置，或通过参数指定其路径；脚本沿用配置中的编译工具集和依赖位置。

先在选定源码提交构建程序，沿用 `scripts/Build-IMao.ps1` 的本地依赖配置，并使用 `dotnet publish` 生成自包含 `win-x64` 程序。WinUI 的构建和发布均自动运行 `Stage-UpdateResources.ps1`。`New-ProgramReleasePackage.ps1` 从明确指定的托管发布目录和原生构建目录制作独立发行目录，添加本地 C++ 运行库及许可，执行资源与 IPC 检查，输出程序 ZIP 和同名 `.report.json`。

```powershell
$sourceCommit = git rev-parse HEAD
# 这些目录必须来自上述同一源码构建；每次使用新的输出目录。
& scripts/New-ProgramReleasePackage.ps1 `
  -PublishRoot out/release-publish -NativeRoot x64/Release -LauncherRoot out/release-launcher `
  -OutputRoot out/release-2026.9.9.4 -SourceCommit $sourceCommit `
  -RedistRoot 'C:/VSBuildTools-Current/VC/Redist/MSVC/14.44.35112/x64'

$appRoot = 'out/release-2026.9.9.4/IMao-v2026.9.9.4-windows-x64'
& $dotnet $publisher prepare --app-root $appRoot `
  --private-key $privateKey --public-key Assets/Updates/trusted-keys.json `
  --output out/maps-2026.9.9.4 --sequence 4 --resource-version 2026.9.9.4 `
  --tag v2026.9.9.4 --program-release true --program-zip "$appRoot.zip" `
  --previous updates/stable.json `
  --core-host "$appRoot/IMao-CoreHost.exe"
```

`prepare` 生成 `update.json`、`packages/*.zip`、候选快照、原生检查日志及 `release-report.json`；**只有当本次发布确实引入了新的资源包时**，才另外打包完整离线集合包（`resources-<版本>-offline.zip`）。所有资源包都沿用上一版时不再重复打包——那份集合与上一次发布的内容逐字节相同，重新上传只是几百 MB 的重复流量；需要时用 `--with-offline true` 强制生成。它拒绝覆盖已有输出目录。ZIP 使用固定文件顺序与时间戳，包清单含每个文件大小和 SHA-256。生产准备必须通过实际 CoreHost `--check-resource-snapshot`。发布附件（包括离线集合与程序 ZIP）必须小于 2 GiB，工具超限即拒绝；规模超过此限时需先调整分发方案。[GitHub 附件限制](https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases)

暂存资源时核对地图数据与所选特征包的完整文件名清单。重复构建可沿用内容一致的输出目录；若源文件已删除而输出仍有旧文件，会停止并要求换用新的输出目录，保留旧文件，不自动删除或把旧资料签入新资源包。可执行 `scripts/Test-ResourceUpdateStaging.ps1 -OutputRoot out/staging-test-new-run` 检查 Windows PowerShell 5 的重复暂存和旧文件拒绝行为。

暂存入口统一在 Windows PowerShell 5.1 中执行。即使从 PowerShell 7 的打包脚本调用，也会自动委派并保留参数和工作目录，保证程序内置数据与相同版本离线包逐字节一致，不受两种 PowerShell JSON 序列化差异影响。

后续资源版本使用更高的 `--sequence` 与四段 `--resource-version`，通过 `--previous` 传入当前正式签名清单；可使用 `--notes-file`、`--min-app-version` 和 `--max-app-version`。内容完全相同的包沿用旧版本及下载地址，只发布变化包。不要给仅资源发布添加 `--program-release true`。保留其他仍受支持的基础资源版本在原清单中，`prepare` 自动保留其他基础资源的快照条目。

`--notes-file` 的内容是**面向玩家的更新公告**，同时进入两处：签名清单的 `app.notes`（客户端更新提示直接显示）与发行页正文。所以按公告写——第三人称陈述程序行为，不要出现"你/您"，也不要写只有开发者才看得懂的内部细节或本次会话的测试过程。两处的可修改性不同：发行页正文随时可用 `gh release edit v<版本> --notes-file <文件>` 改写，而 `app.notes` 一旦签名就不能原地替换——已发布发行页上的同名附件字节不同会被发布脚本拒绝——只能等下一版覆盖，除非另起一个更高的 `--sequence`。

当前云发布使用分片，另附完整安装 ZIP。兼容本地 `prepare` 支持整包输入；本地改为分片验包时把 `--program-zip` 换成 `--program-shards true`：`prepare` 直接从 `--app-root` 读出权威文件清单并按 `ShardMap` 的路径规则切分，签名清单里的整包字段指向一个小的分片描述符，客户端只下载变化的片。

```powershell
& $dotnet $publisher prepare --app-root $appRoot `
  --private-key $privateKey --public-key Assets/Updates/trusted-keys.json `
  --output out/maps-2026.9.10.1 --sequence 2 --resource-version 2026.9.10.1 `
  --tag v2026.9.10.1 --program-release true --program-shards true `
  --previous updates/stable.json `
  --core-host "$appRoot/IMao-CoreHost.exe"
```

`--app-root` 里出现未归类的新文件会直接失败（必须先在 `tools/UpdatePublisher/ShardMap.cs` 里显式归类，避免它悄悄落进大分片）。片 zip 用固定条目顺序与时间戳打包，内容不变的片在本地重打一遍并断言字节一致后**沿用上一版的 URL 与资产名**，只有变化的片指向本次 tag。签名前 `prepare` 会把所有片重新组装回一棵树并交给原有的程序包验收（`VerifyDirectoryAsync`），不通过就不签名；`release-report.json` 的 `program.shards` 列出每个片的名字、大小与哈希。发布脚本逐片按签名清单绑定后上传。

```powershell
& $dotnet $publisher prepare --app-root $appRoot `
  --private-key $privateKey --public-key Assets/Updates/trusted-keys.json `
  --output out/maps-2026.9.10.1 --sequence 2 --resource-version 2026.9.10.1 `
  --tag maps-2026.9.10.1 --previous out/maps-2026.9.9.1/update.json `
  --notes-file out/map-release-notes.md --core-host "$appRoot/IMao-CoreHost.exe"

& $dotnet $publisher verify --input out/maps-2026.9.10.1 `
  --public-key Assets/Updates/trusted-keys.json
& $dotnet $publisher self-test --output out/publisher-test-new-run
```

`shard-map` 在真实暂存根上检查程序分片的路径规则：未识别路径会直接失败（新增文件必须先在 `tools/UpdatePublisher/ShardMap.cs` 里显式归类，不允许悄悄落进大分片），同时校验每个片非空、必需程序文件都存在，并打印每片的文件数与字节数。

```powershell
& $dotnet $publisher shard-map --app-root $appRoot --report out/program-shards.json
```

分片边界、决策与实施阶段见 [ProgramShardIncrementalPlan.md](architecture/ProgramShardIncrementalPlan.md)。

新资源必须先同步到构建输入，再重新生成暂存资源快照。`prepare` 读取指定 `app-root`，不会联网抓取上游地图，也不会推断或自动开放新地图。离线包包括目标基础资源对应快照所需的全部可更新包；其他基础资源客户端需要与其程序兼容的离线包。

## 正式发布与恢复

唯一生产入口是 [CloudRelease.md](CloudRelease.md)：抬版本与公告 → 提交并 push → dispatch `publish=true confirm_version=<版本>` → 构建与临时密钥预演 → production 审批后预留 sequence → 本机 `Complete-CiRelease.ps1` 双签名 → 再审批发布。上文 `prepare` 仅用于兼容与本地验包，不能代替已登记请求、原始响应及冻结产物授权。

`scripts/Publish-ResourceUpdate.ps1` 默认 `Publish=false`；正式执行必须显式授权版本及事务。`release-state.json` 记录最高已分配序号和活动事务；已分配序号永不复用。公开验证通过后，`stable.json`、`channel-state.json` 和完成事务在同一个 Git commit 中推进。中断应恢复原事务、原签名与原附件，不因为 stable 尚未推进就重新签名或分配新序号。只有尚未开始发布的等待签名事务可以显式放弃。

上传前仍检查同版本包内容身份、基础资源兼容、逐片长度/哈希及描述符绑定。已发布同名附件不同字节会拒绝覆盖；不能给 immutable Release 补写缺少附件。`-ProgramZip` 对应整包程序清单，与分片互斥；分片的完整安装 ZIP 使用 `-ManualInstallZip`，除同名 `.report.json` 外还必须在双签名批准的附件清单中，缺省时仅自动选取其中唯一获准安装包。

公开网络检查失败不会推进 stable；排查后重跑失败发布 Job，复用相同事务和字节。网络/代理诊断不得以重复下载大包代替已存在证据：先用本地重组产物及报告对账远端 digest，任何超过约 100 MB 的下载先征求用户同意。

### 分片迁移历史与现行验收

`2026.9.20.1`（sequence 16）曾用整包把认识分片的客户端送到既有安装；`2026.9.20.2`（sequence 17）是首个分片版。这个过渡已完成，不是当前还要重做的步骤。当前流水线固定产出分片和完整安装 ZIP，首次安装使用完整 ZIP，后续程序更新仅下载需要的分片。旧方案中的“可选不传完整安装包”不代表当前生产工作流。

分片绑定可通过 `scripts/Test-ProgramShardReleaseAssets.ps1 -OutputRoot <新的证据目录> -RealPreparedRoot <准备目录>` 本地预览，不联网、不发布。离线集合包只在资源集合变化或显式请求时生成；它服务资源导入，不是程序分片的替代物。

历史增量实测及体积见 [ProgramShardIncrementalPlan.md](architecture/ProgramShardIncrementalPlan.md)，不能把 2026-09 的 MB 数当当前下载预算。编译、自检、签名验包、离线导入与回归报告不代替 WinUI 管理员启动、设置页更新/回退、玩家记录保留及实际游戏定位验收。归档大产物前用 `Compact-ReleaseArtifacts.ps1 -WhatIf` 预览，并保留摘要、报告与必要恢复字节。

## 本地预览版迁移与快照兼容

从 `2026.9.9.3` 开始，已安装的外部快照使用 `formatVersion=2`，记录签名清单的 `minAppVersion` / `maxAppVersion`。每次启动及回退均重新核对程序版本；WinUI 与 CoreHost 都执行此限制。内置快照仍为 v1。旧预览版 `.1` / `.2` 会拒绝 v2；新程序对缺少程序版本约束的旧外部 v1 快照整体恢复内置资源，可重新导入签名离线包，不删除历史目录或个人记录。

历史上首次正式候选预留清单序号 3，高于本地验收包使用过的序号 1 / 2。发布前仍需读取实际线上稳定清单和 `updates/channel-state.json`，序号必须高于两者的最大值，不能覆盖同序号的不同内容，也不能重用已发布的序号。相关修复和候选发行说明见 [Release-2026.9.9.3.md](releases/Release-2026.9.9.3.md)。
