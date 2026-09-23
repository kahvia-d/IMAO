# 程序与地图资源更新

使用者首次手动安装带更新功能的完整程序包。2026.9.9.4 起包含独立启动器，程序与资源均可在设置的“版本与地图资源”中下载。新版程序下载并校验完成后，可以继续使用，也可以点击“退出并更新”；正常退出后再打开也会启用。不自动下载、不强制重启。旧清单未提供程序包签名信息，或新版要求更高的启动器协议时，按钮仍打开正式发行页。离线资源使用“导入离线包”选择 `resources-四段版本-offline.zip`。回退在下次启动生效。程序自更新布局与故障恢复详见 [ProgramUpdates.md](ProgramUpdates.md)。

更新不会包含或覆盖完成记录、路线、筛选选择与个人设置。首次启用没有确认成功时，下次启动恢复上一成功快照。错误签名、损坏资源、下载失败或磁盘不足均保留当前资源。

已安装资源放在 `%LOCALAPPDATA%\IMao-WinUI\ResourceUpdates` 的版本目录。首版保留历史已安装包，不自动清理这些目录；下载与安装的临时文件由事务结束时清理。不要手动删除运行中、待启用或回退所需的包。

## 资源与版本约定

- `Version.props` 是程序版本与基础资源 ID 的唯一来源，版本为 `2026.9.9.1` 形式的四段数字；基础 IMF/IMX 等规则变化时提高基础资源 ID，并发布程序版本。
- `build-info.json` 在构建输出生成，记录实际 Git 提交、程序版本、基础资源 ID、`sourceDirty` 及 `sourceTreeSha256`（提交 SHA 加本地变化文件的内容摘要）。正式发布必须从已提交的明确源码构建；尚未提交的工作区构建可制作正式密钥签名的本地验收包，但远端发布脚本会拒绝，不把它误报为该提交的正式发行。
- 基础资源继续随程序提供。一个 `map-data` 包完整包含八个现有场景的点位、分类、图标、校准、场景准入和本地攻略数据；`tile` 和 `candidate` 分别按地区打包。五份原先嵌入 DLL 的点位在构建时复制到输出的 `Assets/KuroMap/runtime`，只统一既有两个分类 ID 别名，不改变点位 ID、坐标或状态 ID。
- 构建生成 `Assets/Updates/bundled-snapshot.json`，启用 `map-data`、`kuro-tile-packs.json` 中注册的每个瓦片包，以及 `candidate-packs.json` 中注册的候选包。一个包只有在 `referenceVerification.passed` 为真时才会进入快照；目录存在本身不足以启用它。`requiresGameValidation` 为真的场景还必须在 `Assets/KuroMap/scene-validation.json` 中获得批准：客户端始终传 `--resource-snapshot`，因此 `RuntimeFeatureRepository` 会因任何一个已注册包未获批准而中止整个资源加载，表现为“启动核心失败”。打包还需通过 CoreHost 资源预检。
- 内置包的版本必须与线上清单使用同一内容身份：某包内容与上一份 `updates/stable.json` 中同 ID 的包逐文件一致时沿用清单里的版本，内容有变化时才使用本程序版本（暂存入口默认读取仓库的 `updates/stable.json`，可用 `-PreviousCatalog` 覆盖）。版本一致时客户端会把随程序内置的资源识别为已安装，不重复下载；不一致会让客户端认为这些包缺失并重新下载整套资源。客户端同时要求发布内容全部内置时才不提示资源更新。
- 客户端固定读取 `https://raw.githubusercontent.com/kahvia-d/IMAO/main/updates/stable.json`。程序版本、清单序号、资源快照版本和各包版本分别处理。单独发布资源时保留上一清单的程序信息。
- **仓库改过名**（`kahvia-d/WWMAP-TOOLS` → `kahvia-d/IMAO`），而改名**之前**签名的清单里全是旧 slug 的下载地址。GitHub 对这些旧地址仍然重定向，所以客户端与发布器的地址校验**同时接受新旧两个 slug**（`UpdateSignature.ReleasePathPrefixes`、`UpdatePublisher.AcceptedRepoSlugs`）；**新发布一律写新 slug**。若日后要停用旧 slug，必须先确认线上清单里已没有任何旧地址，否则已安装的客户端会拒绝自己的更新渠道。
- 客户端按发布密钥分别记录该渠道已验证过的最高清单序号与内容哈希，本地测试密钥或预览渠道不会污染正式渠道。只有清单所属程序版本早于当前运行版本时（即回放旧清单的场景）序号下降才会被拒绝；同一程序线内的序号下降或同序号不同内容会在记录中重新同步，并在界面与更新日志里说明。旧版客户端写入的单一全局序号由下一次检查时首个验证通过的渠道继承，不会被丢弃。确实需要重建记录时，可在设置中使用“修复更新状态”，该操作仍会用内置公钥验证当前线上清单。
- 私钥保存在仓库外，以 Windows 当前用户 DPAPI 加密；客户端只携带 `Assets/Updates/trusted-keys.json`。清单使用 P-256/SHA-256、P1363 签名，封装字段为 `keyId`、Base64 `payload`、Base64 `signature`。私钥换机须先规划密钥迁移与客户端公钥升级；不要删除旧密钥后重新初始化同名密钥。

## 维护者首次配置

在仓库根目录使用 PowerShell 7。本仓库提供的本地 SDK 路径是示例；其他维护机器可将 `$dotnet` 指向自己的 .NET 8 SDK。

```powershell
$dotnet = Join-Path (Get-Location) 'tools/dotnet-sdk-8.0.424/dotnet.exe'
$env:DOTNET_CLI_HOME = Join-Path (Get-Location) 'third_party/dotnet-cli-home'
$env:NUGET_PACKAGES = Join-Path (Get-Location) 'third_party/nuget-packages'
& $dotnet build tools/UpdatePublisher/UpdatePublisher.csproj -c Release
$publisher = 'tools/UpdatePublisher/Release/net8.0/UpdatePublisher.dll'
$privateKey = Join-Path $env:LOCALAPPDATA 'WWMAP-TOOLS-Publisher/release-signing-key.json'
```

**私钥路径有陷阱，别照抄上一行就去用。** `init-key` 生成在哪个目录，取决于当时是谁在跑：普通桌面 shell 里 `%LOCALAPPDATA%` 就是 `C:\Users\<你>\AppData\Local`；而在 MSIX/AppContainer 封装的打包环境里，写这个路径会被重定向到容器的私有位置：

```text
C:\Users\<你>\AppData\Local\Packages\<包名>\LocalCache\Local\WWMAP-TOOLS-Publisher\release-signing-key.json
```

两个位置同名不同地，`Test-Path` 在错误的那一边只会说"没有"，看起来像密钥不存在。**先确认密钥到底在哪一边**，再把该路径传给 `prepare`：

```powershell
# 两个候选都查一遍，用存在的那个
$candidates = @(
  (Join-Path $env:LOCALAPPDATA 'WWMAP-TOOLS-Publisher/release-signing-key.json'),
  (Get-ChildItem (Join-Path $env:LOCALAPPDATA 'Packages') -Recurse -Depth 6 `
     -Filter 'release-signing-key.json' -ErrorAction SilentlyContinue).FullName
)
$privateKey = $candidates | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
if (-not $privateKey) { throw '找不到发布私钥；先在生成它的那个环境里定位，不要重新 init-key。' }
$privateKey
```

深度必须够：重定向路径在 `Packages` 以下有 5 层，`-Depth 4` 会**静默什么都找不到**（`-ErrorAction SilentlyContinue` 把无结果也吞了），于是又变成"看起来没有密钥"。若这个脚本报了找不到，先用 `Get-ChildItem -Recurse -Filter 'release-signing-key.json'` 不带深度限制手工确认一次。

如果确实一个都搜不到，**不要重新生成**——见下面关于 DPAPI 的说明；改为在生成密钥的那个 Windows 账户下执行发布。

再另外：`init-key` 只在从未建立过发布身份时运行。密钥已存在时**不要重跑**，任何已有的私钥都要先确认位置再使用。

仅在从未建立过发布身份时运行以下命令。目标文件存在会拒绝覆盖。生产私钥不能位于仓库内；测试密钥使用不同 ID、输出目录并显式传入 `--test true`，正式验包和发布拒绝测试密钥。

```powershell
& $dotnet $publisher init-key --repo (Get-Location).Path `
  --private-key $privateKey --public-key Assets/Updates/trusted-keys.json `
  --key-id wwmap-production-2026
```

公钥需要随首次客户端一同交付并提交源码。切勿提交私钥，亦不要把开发者的 `%LOCALAPPDATA%` 或 `%APPDATA%` 目录复制进发行包。

## 构建、验包与离线包

正式候选使用 `scripts/Build-ReleaseCandidate.ps1`，要求当前工作区干净且 `SourceCommit` 等于当前 HEAD。脚本读取已经验证的 VS 2022 原生配置，在新的 CMake 目录中重新编译 CoreHost 和原生测试，重建 WinUI 后生成自包含发布，并输出独立程序 ZIP。每个阶段检查源码未变化；原生构建收据记录提交、版本与实际 CoreHost SHA-256，正式打包必须核对收据，不能拿旧 EXE 搭配新清单。

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

程序发布默认仍用整包 ZIP。改为分片发布时把 `--program-zip` 换成 `--program-shards true`：`prepare` 直接从 `--app-root` 读出权威文件清单并按 `ShardMap` 的路径规则切分，签名清单里的整包字段指向一个小的分片描述符，客户端只下载变化的片。

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

分片边界、决策与实施阶段见 [ProgramShardIncrementalPlan.md](ProgramShardIncrementalPlan.md)。

新资源必须先同步到构建输入，再重新生成暂存资源快照。`prepare` 读取指定 `app-root`，不会联网抓取上游地图，也不会推断或自动开放新地图。离线包包括目标基础资源对应快照所需的全部可更新包；其他基础资源客户端需要与其程序兼容的离线包。

## 正式发布

准备输出本身不会发布。检查完整构建、更新事务测试、资源预检和地图回放报告后，再执行下面的独立发布入口。首次或程序更新时增加 `-ProgramZip`；仅资源发布省略它。GitHub CLI 必须以有本仓库写权限的维护者身份登录。

**发布前先确认自己在哪个分支。** 候选包、公告文件和 `prepare` 的输入都按路径读取；如果在这中间有别的会话或自动任务 `checkout` 了其它分支，路径会找不到，`prepare` 会失败（此时尚未产生任何签名产物，重新切回并重跑即可，不用清理）。动手前跑一次：

```powershell
git branch --show-current     # 必须是你要发布的那个分支
git log --oneline -1          # 必须与 -SourceCommit 一致
```

```powershell
& scripts/Publish-ResourceUpdate.ps1 -PreparedRoot out/maps-2026.9.9.1 `
  -NotesFile out/program-release-notes.md `
  -ProgramZip out/release-2026.9.9.1/IMao-v2026.9.9.1-windows-x64.zip
```

脚本固定使用 `kahvia-d/IMAO`（仓库改名前的 slug 为 `kahvia-d/WWMAP-TOOLS`）：校验本地签名与文件 → 检查稳定清单序号 → 创建或复用草稿 → 上传缺少附件 → 下载核对草稿字节 → 发布 → 无认证公开下载核对 → 最后通过 GitHub 文件 API 更新 `updates/stable.json`，并回读核对。更新稳定清单使用旧文件 SHA，遇到并发发布会失败，重新读取并重新准备后再发布。相同名称的远端附件内容不同会拒绝覆盖；已发布版本缺附件也会拒绝补写。

已发布的序号由 `updates/channel-state.json` 持久化。脚本在创建草稿之前核对它，并在推进稳定清单**之前**写入新序号，因此已发布的序号永不重用：即使 `updates/stable.json` 被回退或历史被重写，客户端已经记录过的高序号也不会重新可用（客户端只记得自己验证过的最高序号，重编号会让所有旧客户端拒绝更新）。稳定清单推进失败时该序号已被占用，需用更高的 `--sequence` 重新准备发布，而不是重试同一个序号。

"无认证公开下载核对"这一步用 .NET `HttpClient` 拉取附件并比对哈希，它只按环境变量与系统代理设置走代理，不跟随只在网络层生效的透明代理：系统代理关闭、而本机代理只做透明转发时，这一步会直连 GitHub 并被远端重置（`An error occurred while sending the request`，内层是 `远程主机强迫关闭了一个现有的连接`）。此时发行版与附件其实已经上传并公开，只是稳定清单没有推进；让 .NET 也走代理后重跑同一准备目录即可，不需要重新签名：

```powershell
$env:HTTPS_PROXY = 'http://127.0.0.1:7890'; $env:HTTP_PROXY = $env:HTTPS_PROXY
```

创建草稿之前，还会比较已验证的线上稳定清单与候选清单：共同包 ID 与版本的归档哈希、大小、类型、完整文件清单必须一致；已有基础资源版本的条目不得丢失，程序版本不得倒退。忘记 `--previous` 时不能绕过这些检查。可执行 `scripts/Test-ResourceCatalogTransition.ps1 -OutputRoot out/catalog-transition-test-new-run` 运行本地清单迁移回归。

分片程序发布同样在创建草稿前完成本地绑定：`scripts/ResourceUpdateAssets.ps1` 逐个核对签名清单里每个分片与描述符的本地长度和 SHA-256，只上传指向本次 tag 的归档；未变分片保留它已发布的旧 URL，并和"只带过去程序"的资源-only 发布一起进入公开可达性检查。`-ProgramZip` 与分片发布互斥。可执行 `scripts/Test-ProgramShardReleaseAssets.ps1 -OutputRoot out/program-shard-assets-new-run` 运行本地绑定回归；带上 `-RealPreparedRoot <准备目录>` 可以在上传前预览真实归档的上传清单（它会读取该目录的 `update.json` 与 `release-report.json`，只做本地校验，不联网）。

### 过渡版与分片版的发布差异

首个带分片能力的客户端必须先到达用户手里：线上 `2026.9.19.2` 是在分片实现之前构建的，它不认识 `Shards`。所以**过渡版仍按整包发布**（清单里没有 `Shards`），所有客户端会自动升级到"认识分片的客户端"；**再下一个版本**才用分片。两条路线都必须在发行页保留一份完整 zip，因为分片时代的新用户首次安装只能靠它。

发布前逐项确认（本机现状写在这里，动手前重跑一遍）：

```powershell
git branch --show-current                 # 必须是发布分支
git status --short                        # 必须为空：候选构建要求干净工作区
git log --oneline -1                      # 与 -SourceCommit 一致
(Get-Content Version.props -Raw) -match '<IMaoVersion>([^<]+)</IMaoVersion>' | Out-Null; $Matches[1]   # 新版本号
$privateKey = Join-Path $env:LOCALAPPDATA 'WWMAP-TOOLS-Publisher/release-signing-key.json'
Test-Path -LiteralPath $privateKey        # 必须是 True；找到的是 DPAPI 加密的生产私钥
```

`$privateKey` 失效时按上文「找不到发布私钥」一节搜索（重定向路径在 `Packages` 下 5 层），**不要重新 `init-key`**。本机实测位置与验证方式记录在 [MapRegionOnDemandHandoff.md](MapRegionOnDemandHandoff.md) 的「私钥位置」一节：主位置的那份与 `Assets/Updates/trusted-keys.json` 里的 `wwmap-production-2026` 逐字节匹配，DPAPI 当前用户可解封。

**过渡版（首个带分片能力的版本，推荐路线）**——`prepare` 用整包输入：

```powershell
$appRoot = 'out/release-<版本>/IMao-v<版本>-windows-x64'
& $dotnet $publisher prepare --app-root $appRoot `
  --private-key $privateKey --public-key Assets/Updates/trusted-keys.json `
  --output out/maps-<版本> --sequence 16 --resource-version <版本> `
  --tag v<版本> --program-release true --program-zip "$appRoot.zip" `
  --previous updates/stable.json --core-host "$appRoot/IMao-CoreHost.exe"
& $dotnet $publisher verify --input out/maps-<版本> --public-key Assets/Updates/trusted-keys.json
& scripts/Publish-ResourceUpdate.ps1 -PreparedRoot out/maps-<版本> -NotesFile out/program-release-notes.md `
  -ProgramZip "$appRoot.zip"
```

**分片版（过渡版之后的常规发布）**——`prepare` 用分片输入，发布时另外附一份完整 zip 供新用户首次安装：

```powershell
& $dotnet $publisher prepare --app-root $appRoot `
  --private-key $privateKey --public-key Assets/Updates/trusted-keys.json `
  --output out/maps-<版本> --sequence <上一个+1> --resource-version <版本> `
  --tag v<版本> --program-release true --program-shards true `
  --previous updates/stable.json --core-host "$appRoot/IMao-CoreHost.exe"
& $dotnet $publisher shard-map --app-root $appRoot --report out/maps-<版本>/shard-map.json
& $dotnet $publisher verify --input out/maps-<版本> --public-key Assets/Updates/trusted-keys.json
# 上传前预览：每个分片与描述符都按签名清单核对本地字节，并列出真正要上传的归档
& scripts/Test-ProgramShardReleaseAssets.ps1 -OutputRoot (Join-Path 'out' ('shard-preview-' + [guid]::NewGuid().ToString('N'))) `
  -RealPreparedRoot out/maps-<版本>
& scripts/Publish-ResourceUpdate.ps1 -PreparedRoot out/maps-<版本> -NotesFile out/program-release-notes.md `
  -ManualInstallZip "$appRoot.zip"
```

`-ProgramZip` 与分片发布互斥；分片发布时给了 `-ProgramZip` 会直接拒绝。`-ManualInstallZip` 不写进签名清单（清单里是分片与描述符），它与本次发布的绑定靠同名 `.report.json`（`passed`、`sourceDirty=false`、`sourceCommit`、`version`、大小与 SHA-256 全部核对）。没有 `-ManualInstallZip` 时脚本会提示"本次没有任何可供全新安装的归档"。那份 zip 不必每个版本都重传：任何一份认识分片的完整 zip 都能自动增量升级到最新。

离线集合包按同一原则处理：`release-report.json` 的 `offline` 为 `null` 时（本次没有任何新资源包）发布脚本不上传它，只会打印"resource set unchanged"，并把上一次发布的离线包作为当前完整的离线交付物继续使用；确实需要一份新的就用 `prepare --with-offline true` 重新准备。

发布后：

```powershell
git pull                                  # 取回脚本刚推进的 updates/stable.json 与 channel-state.json
(Get-Content updates/stable.json -Raw | ConvertFrom-Json).payload   # 看一眼新序号
```

再让客户端实测一次并记录字节数：过渡版应看到普通整包升级；分片版应只在 `ui`(+ 本次改动片) 上花流量（§4 的实测表：最常见 ≈30 MB，改点位/攻略 ≈1.6 MB）。释放大体积证据用 `scripts/Compact-ReleaseArtifacts.ps1`。

如果附件上传或公开下载失败，稳定清单保持原值。排查网络后可对同一准备目录重试，不需要重新签名或替换已有资源。日志和验证输出留在准备目录，不记录私钥或认证令牌。清单推进之后客户端下次检查才会看见更新；已有运行实例仍固定使用当前快照。发布完成后可运行 `scripts/Compact-ReleaseArtifacts.ps1` 归档这些证据并释放候选与准备目录里可重建的大体积产物（`-WhatIf` 先预览，`-RetainFull N` 保留最近 N 个目录完整；归档会逐文件校验哈希并写入 `archive-manifest.json`，`scripts/Test-CompactReleaseArtifacts.ps1` 覆盖归档、保留与越界拒绝）。发布字节本身仍可从对应 GitHub 发行重新下载，并与归档的 `release-report.json` 哈希对账。

编译、自检、签名验包、离线导入与回归报告证明更新系统和已有资源可用，不能替代新地区的实际游戏定位验证。任何新区准入仍需既有校准、资源与实机证据。

## 本地预览版迁移与快照兼容

从 `2026.9.9.3` 开始，已安装的外部快照使用 `formatVersion=2`，记录签名清单的 `minAppVersion` / `maxAppVersion`。每次启动及回退均重新核对程序版本；WinUI 与 CoreHost 都执行此限制。内置快照仍为 v1。旧预览版 `.1` / `.2` 会拒绝 v2；新程序对缺少程序版本约束的旧外部 v1 快照整体恢复内置资源，可重新导入签名离线包，不删除历史目录或个人记录。

首次正式候选预留清单序号 3，高于本地验收包使用过的序号 1 / 2。发布前仍需读取实际线上稳定清单和 `updates/channel-state.json`，序号必须高于两者的最大值，不能覆盖同序号的不同内容，也不能重用已发布的序号。相关修复和候选发行说明见 [Release-2026.9.9.3.md](Release-2026.9.9.3.md)。
