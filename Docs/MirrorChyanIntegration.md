# 接入 Mirror酱（MirrorChyan）分析与方案

> 结论摘要：Mirror酱 **可以接入，但不能单独解决"没 VPN 的玩家"这个问题**。
> 它的检查更新接口免费且国内可达（有价值），但**下载是有偿的**（需用户购买 CDK），
> 而且**一个资源 ID 只能对应一个压缩包**，装不下我们"程序分片 + 多个地区包"的更新集合，
> 也**没有托管任意文件的接口**，因此它**承载不了我们那份签名清单 `updates/stable.json`**。
>
> 推荐做法：把 Mirror酱当成 **①免费的更新通知渠道** + **②签名清单之外的一条"不可信传输层"**
> （下载来的字节一律按签名清单逐文件校验），而不是替换现有的更新机制。
> 真正卡住国内玩家的是 `stable.json` 这一跳，需要另外给它加国内可达的镜像。

## 1. Mirror酱的计费模型（决定了它能做什么）

官方文档只写了"url 仅在有新版本且 CDK 有效时返回"，没有直说下载要钱。实测与第三方接入方文档可以确认：

- **检查更新接口免费、无限次**：不传 `cdk` 也能拿到 `version_name` / `version_number` / `channel` / `release_note`。
- **下载有偿**：不传 CDK 时响应里**根本没有 `url` 字段**。March7thAssistant 的官方 FAQ 写得很直白：
  > 它为我们提供了免费的检查更新接口，但它的下载是有偿的，需要用户付费使用。
  > 不过，即使不购买 Mirror 酱的下载服务，你也可以在检测到更新后，在设置里选择从海外源（GitHub）下载。
- **上传侧免费**：开发者用 `MirrorChyan/uploading-action` 上传，Token 放仓库 secret；收益与开发者共享。

**对本项目的含义**：接入 Mirror酱后，没 VPN 且不打算付费的玩家仍然下载不了，只是**从"完全不知道该更新"变成"知道有新版本，并被引导到可用的下载入口"**。要让这部分玩家真的能更新，必须另外给他们一条免费的、国内可达的整包下载路径（见 §6）。

## 2. 现状盘点：我们的更新链路和它的硬约束

| 环节 | 现状 |
| --- | --- |
| 清单入口 | `UpdateService.StableUri` = `https://raw.githubusercontent.com/kahvia-d/IMAO/main/updates/stable.json`（单一 URL） |
| 清单格式 | P-256/SHA-256 签名封装（`keyId` + Base64 `payload` + `signature`），私钥在仓库外 DPAPI 加密，客户端固定公钥 |
| 清单内容 | `UpdateCatalog`：`sequence`、`app`（程序版本 + 程序包 + 分片）、`resources`（按基础资源/程序版本范围过滤的快照与地区包） |
| 下载地址校验 | `UpdateSignature.ValidateUrl`：只允许 `github.com` / `release-assets.githubusercontent.com` / `objects.githubusercontent.com` / `raw.githubusercontent.com`，且 GitHub 路径必须落在 `/releases/` 内 |
| 程序更新 | 分片发布：`ProgramUpdateStore.AssembleAsync` 只为"内容与本机不同"的分片下载，其余从当前版本目录复制并逐个核对 SHA-256 |
| 安装方式 | **不覆盖运行中的程序**：装进 `ProgramUpdates/versions/<版本>-<包摘要>/app/`，由启动器试运行 + 启动确认后提交 |
| 最终校验 | `ProgramPackageValidation.VerifyDirectoryAsync`：目录里的文件必须与签名清单 `files` **完全一一对应**（多一个、少一个、大小或哈希不符都拒绝），`build-info.json` 还要与清单里的版本/基础资源/提交一致 |
| 版本号 | 四段纯数字 CalVer（如 `2026.9.25.2`），`UpdateSignature.RequireVersion` 强制校验，**不是 SemVer** |

### 2.1 一个必须先说清楚的事实：卡住玩家的不只是"下载"

`stable.json` 本身也在 GitHub 上（`raw.githubusercontent.com`）。国内无代理的环境下这一跳大概率失败，
所以现状是**"检查更新"和"下载"一起挂**。Mirror酱解决不了这一跳（见 §3），
所以"接入 Mirror酱"这件事**必须**和"给清单本身加一条国内可达的镜像"一起考虑，否则接入后
国内玩家看到的仍然是"检查更新失败"。

> 本机 `Resolve-DnsName mirrorchyan.com` 返回 `198.18.1.12`，`release-assets.githubusercontent.com`
> 返回 `198.18.0.213` —— 这是代理工具典型的 fake-IP 段，说明**开发机是挂了代理的**，
> GitHub 在本机能通，玩家那边不能。不要用开发机的连通性判断玩家的连通性。

## 3. 关键判断：我们的安全边界是"签名清单 + 逐文件哈希"，不是"压缩包哈希"

这条判断决定了整个接入方式，值得写清楚：

- 载荷可信度来自**固定 P-256 公钥对清单的签名**，加上**清单内记录的每个文件的大小与 SHA-256**。
  URL 主机白名单只是传输卫生，**不是安全边界**（`Docs/ResourceUpdates.md` §17 已经把这条写死了）。
- 程序包的安装路径**最后一定要过 `VerifyDirectoryAsync`**，而它的语义是"目录内容 == 签名清单的 `files`"。
  也就是说：**换个来源拿到同样的文件，安全性和从 GitHub 拿完全等价。**
- 推论一：Mirror酱的重打包增量包（字节与我们的任意一个归档都不同）**可以直接用**——
  解压后按签名清单逐文件校验即可，不需要它的 `sha256` 是对的。
- 推论二：Mirror酱那套 `changes.json` 的 `deleted` / `deleted_dir` **我们完全不需要实现**——
  我们是按签名清单"重建"目标目录（只放清单里有的文件），被删除的文件天然不会被放进去，
  而残留文件会被 `VerifyDirectoryAsync` 拒绝。

**因此 Mirror酱的正确位置是：签名清单之外的一条传输层。** 它拿到的东西一律不可信，
每一个文件都要对签名清单负责；传输失败就退回 GitHub 分片，玩家无感。

推论二还带来一个额外好处：**不要把 mirror 域名写进签名清单。**

### 3.1 为什么不要把 Mirror酱地址签进清单（顺序陷阱）

`ValidateCatalog` 会在验签之后**整份拒绝**含不受信任主机的清单。如果我们先改了发布侧、把
`https://mirrorchyan.com/...` 签进 `stable.json`，那么**所有还没更新到新客户端的旧客户端会连整份清单都拒收**，
直接从"下载慢"变成"更新全挂"——这正是 `Docs/ResourceUpdates.md` 里记的那次仓库改名的自锁事故的翻版，
区别只是这次是域名白名单。

避免它的办法很简单：**清单里继续只出现 GitHub 地址；Mirror酱的下载地址在运行时向它的 API 现问现用**，
用一个只有新客户端才认识的独立下载通道去取，旧客户端完全不受影响。这样：

- 清单 schema、`ValidateUrl`、`AllowedHosts` 都不用动，没有客户端/服务端的版本顺序约束。
- 新通道的 URL 是"不可信输入"，但它指向的字节要在 `VerifyDirectoryAsync` 里对签名清单负责。
- 将来 Mirror酱 倒了、换域名了，只影响新通道，不会污染签名清单。

## 4. 实测到的接口行为（2026-09-25 在本机代理下实测）

```powershell
# 不传 CDK：免费拿到版本信息，但没有 url
Invoke-WebRequest 'https://mirrorchyan.com/api/resources/March7thAssistant/latest?current_version=v1.0.0&user_agent=imao_probe'
```

| 场景 | HTTP | 响应 |
| --- | --- | --- |
| 不传 `cdk` | 200 | `code:0`，`data` = `version_name` / `version_number` / `channel` / `os` / `arch` / `release_note`，**无 `url`** |
| 已是新版（`current_version` == 最新） | 200 | `code:0`，`msg:"current resource latest version is vX"`，**`release_note` 是字符串 `"placeholder"`** |
| `cdk` 无效 | 403 | `code:7002`，`msg:"Please confirm that you have entered the correct cdkey"`，`data` 里**仍然带版本信息** |
| `os=windows&arch=amd64` 但资源不是按平台上传的 | 404 | `code:8001`，`msg:"resource not found"` |
| `res_id` 不存在 | 404 | `code:8001`，`msg:"resource not found"` |

三个必须注意的细节：

1. **`version_name` 带 `v` 前缀**（`v2026.9.25`），我们的程序版本是四段纯数字（`2026.9.25.2`）。
   发请求时要拼出与上传侧一致的格式，否则每次都会退化成全量包。
2. **`release_note` 在"已是最新"时是字面量 `"placeholder"`**，不能直接当公告显示。
3. **`8001` 无法区分"没接入""没上传过""平台参数不匹配"**，运行时的日志要记下 `os`/`arch`/`channel`（不含 CDK），
   按官方文档 §"返回 404（code 8001）时的排查"逐个组合试。

## 5. 推荐方案

### 5.1 分层设计

```text
检查更新
  ├─ 签名清单（权威：版本、兼容范围、分片、逐文件哈希）
  │    来源按顺序尝试：① 现有 raw.githubusercontent.com  ② 新增国内可达镜像（§6）
  └─ Mirror酱 API（免费、国内可达，仅作为"通知"与"CDK 状态"）
       不传 CDK  → 得到 version_name，用来交叉验证清单里的 app.version，并提示"可填 CDK 加速"
       传 CDK    → 额外得到 url/sha256/filesize/update_type，作为程序包的一条下载源
                     CDK 类错误（7001–7005）只影响这条通道，绝不阻断整体更新

下载程序包（PrepareProgramAsync）
  └─ 逐文件填充目标目录，优先级：
       ① 本机当前版本目录里哈希一致的文件（现有能力，零流量）
       ② Mirror酱的 url（有 CDK 且 version_name 与签名清单的 app.version 一致时）
       ③ GitHub 分片（现有能力，兜底）
     组装完成后仍然走 VerifyDirectoryAsync + 原生预检，任何一步不过就整包丢弃
```

### 5.2 客户端改动点（具体到文件）

| 位置 | 改动 |
| --- | --- |
| `IMao-WinUI.Core/Updates/MirrorChyanChannel.cs`（新增） | 纯逻辑，可单测：拼请求 URL、解析响应、错误码→文案、`version_name` 与程序版本的双向换算与比较。不含网络、不碰 UI |
| `IMao-WinUI.Core/Updates/MirrorChyanMirrorSource.cs`（新增） | 把镜像 zip 读成"路径→条目"的只读袋子；只接受签名清单 `files` 里声明过的路径、大小必须一致，拒绝链接/重复/特殊文件/路径穿越（复用 `ProgramPackageValidation.ReadArchiveEntries` 的规则） |
| `IMao-WinUI.Core/Updates/ProgramUpdateStore.cs` | `AssembleAsync` 的文件来源链加入镜像袋子：把现有 `TryReuseShardAsync`（本机复用）抽象成可串联的 provider，镜像袋子插在"本机复用"和"下载分片"之间。**不改 `VerifyDirectoryAsync`、不改分片校验** |
| `IMao-WinUI.Core/Updates/UpdateService.cs` | `CheckAsync` 里并发调用 Mirror酱 API（失败只记录，不抛）；结果并入 `UpdateCheckResult`（新增 `MirrorStatus`）。**`StableUri` 改成 URL 列表**，按顺序尝试（§6） |
| `IMao-WinUI.Core/Updates/UpdateSignature.cs` | **不动**。镜像下载走自己的 HTTP 通道与自己的主机规则（`*.mirrorchyan.com` + https），不并入 `ValidateUrl` 的 GitHub 白名单 |
| `IMao-WinUI.Core/Updates/MirrorChyanSettings.cs`（新增） | CDK 的存取：`ProtectedData`（CurrentUser）加密后落 `%LOCALAPPDATA%\IMao-WinUI\`，**明文不得进日志、配置导出、崩溃报告**；界面掩码显示 |
| `IMao-WinUI/Services/UpdateUiController.cs` | 暴露 `CdkConfigured`、`MirrorMessage`、`SetCdkAsync`/`ClearCdkAsync`、`OpenMirrorPage()` |
| 设置页（`Views/` 下"版本与地图资源"） | CDK 输入框（掩码）+ 保存/清除 + `[Mirror酱](https://mirrorchyan.com/zh/projects?rid=<rid>&source=imao_app_settings)` 跳转链接 |

复用现有设施，不要另起一套：`HttpClient`（`AllowAutoRedirect=false` + 手动重定向 + 超时策略）、
`UpdateJson.Options`、`UpdateStorage` 的原子写与 `RejectLink`、`Audit()` 日志、
`Tests/ProgramUpdates` 已有的假 HTTP handler 测试夹具。

### 5.3 参数约定（必须先跟 Mirror酱确认，不要猜）

| 参数 | 取值 | 说明 |
| --- | --- | --- |
| `res_id` | **待申请** | 联系集成开发 QQ 群 `1026040805` 获取；`Docs` 与官方 Skill 都明确要求不要编造 |
| `current_version` | `"v" + <四段版本>` | 必须与上传时的 `version_name` 同格式，否则永远拿全量包 |
| `user_agent` | `IMAO_APP` | 对应统计面板的「签到源」；`mirrorchyan_web` 是保留值，不能用 |
| `os` / `arch` | **待定**：要么都不传，要么固定 `windows`/`amd64` | 按平台分别上传才传，且必须与上传侧一致；不一致就是 404/8001。我们只有 win-x64，**倾向都不传**，简单且没有 8001 风险 |
| `channel` | `stable` | 我们目前只有稳定渠道 |
| 跳转链接 | `https://mirrorchyan.com/zh/projects?rid=<rid>&source=imao_app_settings` | `source` 对应「付费源」统计 |

### 5.4 版本比较规则

- **权威版本永远是签名清单里的 `app.version`**，Mirror酱的 `version_name` 只用来做"同一版本的确认"。
  两者换算后不相等 → 这条通道本次不用，直接走 GitHub。
- 比较用现有的 `UpdateSignature.RequireVersion`（四段数字），**不要**把 `version_name` 直接当版本比较。
- "已是最新"的判定只看清单，不看 Mirror酱的 `msg`；`release_note == "placeholder"` 时不要显示公告。

### 5.5 增量包的两个坑（源自 MAA 的实现，见 §10）

1. **`url` 不等于增量包。** MirrorChyan 是**收到首个请求后才开始按需打包 OTA** 的，打包期间返回的是完整包。
   必须判 `data.update_type`：`full` 时**等 10 秒重问一次**，仍是 `full` 才当作"没有增量包"，
   并且**在下载约 950 MB 的整包之前先让用户确认**（MAA 用 `_requiresFullPackageConfirmation` 做这件事）。
2. **`os`/`arch` 必须与上传侧的分区一致。** MAA 的 `MAA` 资源是按 `win/x64`、`win/arm64`、`macos/*` 分别上传的，
   所以它每次都带 `os`/`arch`；而 `MaaResource` 不分区，就一个都不带。我们只有 win-x64，
   **要么上传时也按 `os=win&arch=x64` 分区、请求时一并带上，要么两边都不带**——不一致就是 §4 表里的 404/8001。

## 6. 尚未解决、必须拍板的两件事

### 6.1 `stable.json` 的国内可达性（真正的阻塞点）

Mirror酱没有托管任意文件的接口，我们约 720 KB（738,484 字节）的签名清单也塞不进它的字段（`custom_data` 的大小上限未知，
且只在"这个版本被上传过"时才有值，冷启动拿不到）。可选：

| 方案 | 代价 | 说明 |
| --- | --- | --- |
| **（推荐）给清单加一条国内可达的静态镜像** | 一次性配置 + 少量存储费 | `StableUri` 改成有序列表：`raw.githubusercontent.com` → 国内镜像。安全性不变（签名是边界）。旧客户端继续走第一条，不受影响 |
| 让 Mirror酱在 `custom_data` 里带清单 | 需要对方配合 + 体积未知 | 约 720 KB 的 Base64 载荷偏大，可行性需问技术支持 |
| 只在无 CDK 时降级为"通知 + 手动下载" | 最低成本 | 国内玩家看到"有新版本 vX"，点按钮去国内可下载的整包入口手动覆盖安装 |

镜像宿主的选择要点：国内直连稳定、支持按路径取小文件、能接受频繁更新（每次发布都变）。
对象存储（OSS/COS）+ 自定义域名最稳。**注意 jsDelivr 对 `@main` 有较长缓存**，
清单需要"发布即可见"，否则会出现"看到新版本但下载 404"。

#### Gitee 只能放清单，放不了包（2026-09-25 核实）

Gitee 官方帮助中心《创建 Release（发行版）》写明：

| 限额 | 数值 |
| --- | --- |
| 单个附件 | ≤ **100 MB**（GVP 项目 200 MB） |
| 每个仓库附件总量 | ≤ **1 GB**（推荐项目 5 GB；GVP 项目 20 GB） |
| 统计范围 | 附件总容量**包括仓库附件和发行版附件**，共用同一份配额 |

对照我们的产物：

| 产物 | 大小 | 能否上 Gitee |
| --- | --- | --- |
| `updates/stable.json`（签名清单，普通代码文件） | 约 720 KB | ✅ 可以 |
| 最大的程序分片 `assets-tiles` | 653.3 MB | ❌ 超单附件上限 6.5 倍 |
| 整包（七个分片合计） | 949.1 MB | ❌ 超上限，且**一个版本就吃光整仓库 1 GB 配额** |

**这是硬配额，不是政策问题**——Gitee 的额度是按仓库算的，不是按流量算的，所以它天然不能当分发渠道。
分发要另找地方（对象存储按量付费，或依赖 Mirror酱 / 让玩家手动下载）。

清单这一半是可行的：实测 Gitee 的 raw 直链对**非浏览器、未登录、自定义 UA** 的客户端返回 200：

```powershell
# 自定义 UA 模拟更新器，两个公开仓库均 200
Invoke-WebRequest 'https://gitee.com/oschina/git-osc/raw/master/README.md' `
  -Headers @{'User-Agent'='WWMAP-TOOLS/2026.9.25.2'}
```

但要注意清单里现在写的是 GitHub 下载地址：只把清单搬到 Gitee，玩家会变成
**"能看到有新版本，点下载仍然失败"**。要让他真能下载，下载地址也得指向国内可达的宿主——
而包放不下 Gitee（见上表）。这两件事必须一起想。

费用参考（对象存储，国内节点，按量）：存储约 0.12 元/GB/月；外网流出约 0.5 元/GB。
我们分片发布的常见更新只下 `ui` 分片（58.6 MB）≈ **0.03 元/次**；全量 949 MB ≈ **0.5 元/次**。
分片机制在这里是有直接经济价值的。

#### 第一步已实施（2026-09-25）：清单多来源

已落地"检查更新不再单点依赖 GitHub"这一半，不需要资源 ID：

| 位置 | 改动 |
| --- | --- |
| `UpdateService.ManifestMirrors` | 新增镜像列表，`Sources` = `StableUri` + 镜像，按顺序尝试 |
| `UpdateService.DownloadManifestAsync` | 逐来源读取；**只有"够不到"（`HttpRequestException` / `TimeoutException` / `IOException`）才换下一个来源** |
| `UpdateSignature.ValidateManifestHost` | 清单来源自己的主机规则（`gitee.com`、`*.giteeusercontent.com`、`raw.githubusercontent.com`），**与 `AllowedHosts` 分开**——镜像绝不能变成签名清单可以指向的下载地址 |
| `GetResponseAsync` | 加了主机策略与超时两个参数；**只有"后面还有来源"的那次尝试才缩短超时**（12s），最后一次仍是 30s，所以单来源时行为一字未变 |
| `scripts/Set-GiteeMirror.ps1`（新增） | 推送镜像的独立入口；Token 读仓库外的 `%LOCALAPPDATA%\WWMAP-TOOLS-Publisher\gitee-token.txt`，**放在请求体而非 URL**，所以不会被日志带出去；推送后从 API 回读并比对 SHA-256 |
| `scripts/Publish-ResourceUpdate.ps1` | 在 GitHub 推进并回读校验**之后**调用上面这一步；**失败只降级为告警，不影响发布的成败** |
| `Tests/ResourceUpdates` | 新增 5 条：回退生效、可达但不验签的权威来源**不会**被静默换掉、镜像不验签被拒、全部来源失败时抛出的仍是传输异常原类型、发布脚本与客户端地址一致 |

几个刻意的取舍：

- **验签失败不换源。** 镜像只为"够不到"而生；一个能应答但验不过的来源是篡改信号，静默改用镜像会把它藏起来。
- **不受信任的重定向、超限响应同样不换源。** 与上一条同一个理由；`Gitee` 的 302 目标是 `raw.giteeusercontent.com` 的签名短时 URL，所以那台主机必须显式允许。
- **最后一次尝试不缩短超时。** 若给最后一次也设 12s，"慢但能通"的链路会从成功变成失败——这是回归，不是优化。
- **地址写在两处（客户端常量 + 发布脚本），用一条测试锁住一致性。** 只改一处会让最需要镜像的玩家静默失去它。

**这一步之后国内玩家能"检查到更新"，但仍然下不了**——清单里的下载地址还是 GitHub。要让没 VPN 的玩家真正下载，还得做 §6.2 / §8 的第 3 步。

#### 实测：镜像链路本身（2026-09-25，仓库 `tan-xuedong/imao-updates`）

| 项目 | 结果 |
| --- | --- |
| 创建（POST） | ✅ 空仓库上直接建出 `main` 分支与 `stable.json` |
| 更新（PUT，需要现有 blob sha） | ✅ 这是今后每次发布都会走的路径 |
| 推送后回读比对 SHA-256 | ✅ 与本地 `updates/stable.json` 逐字节一致（738,484 字节） |
| 客户端路径（302 → `raw.giteeusercontent.com`） | ✅ 两个主机都通过 `ValidateManifestHost`，取回字节一致 |
| **CDN 新鲜度** | ⚠️ 同一路径推送新内容后，raw **约 81 秒**后才返回新字节 |
| 响应头延迟 | Gitee **2.0–9.0 秒**；GitHub 0.10–0.20 秒 |
| 738 KB 正文传输 | Gitee 冷路径可达 40–100 秒，热路径约 2 秒（351 KB/s） |

几点解读：

- **81 秒的滞后可以接受**，但要知道它存在：发布后约 1.5 分钟内检查更新的国内玩家可能仍看到上一版清单。自动检查有 24 小时节流，所以最坏情况是"当次没看到、下次才看到"；手动点"检查更新"即可立刻重试。
- **30 秒超时是安全的**：客户端的 30 秒只覆盖"拿到响应头"，实测 Gitee 是 2–9 秒。慢的是正文，而正文由 60 秒**空闲**超时约束（`ReadWithTimeoutAsync`），只要数据在持续到达就不会失败。
- **以上数字全部经由本机代理测得**（`gitee.com` 在本机解析到 `198.18.x.x`，是代理的 fake-IP 段），所以**不代表国内玩家的真实速度**。上线前应当在真实国内网络（无代理）上跑一次 §9 的清单，确认单次检查的耗时可接受。
- 顺带记一笔：720 KB 的清单对"只是想问一句有没有新版本"来说偏大。将来若国内链路仍然偏慢，可以考虑让检查先取一个很小的"版本指针"文件，再按需拉完整清单——这属于后续优化，不在第一步范围内。

### 6.2 地区包怎么办

Mirror酱是**一个 `res_id` 对应一个压缩包**（`uploading-action` 的 `filename` 是单个 glob），
而我们的更新集合是"程序分片 + `map-data` + `map-icons` + 若干 `tile`/`candidate` 包"。
不能指望一个 `res_id` 装下：

- **程序包（含内置基础资源）**：适合放 Mirror酱，也是玩家最痛的那一大坨。
- **地区包**：每个都要单独的 `res_id`，而且每次下载都算用户的每日配额。
  建议**先不接**，地区包继续走 GitHub，等 §6.1 的国内镜像做好后一并受益；
  或者将来为"地图数据包"单独申请一个 `res_id`。

## 7. 发布侧接入

上传用官方 action（`MirrorChyan/uploading-action@v1`），不要自己写上传逻辑——官方文档明确说
定制化的 CI/CD 由他们 PR，并且我们的"多归档 + 分片"属于非标准场景，应当先跟技术支持对齐。

```yaml
# .github/workflows/mirrorchyan-upload.yml（示意）
- uses: MirrorChyan/uploading-action@v1
  with:
    filetype: local                 # 我们的分片发布不一定每个版本都有整包 release 资产
    filename: <本机重新组装出的整包 zip>
    version_name: v<四段版本>         # 与客户端 current_version 的格式必须一致
    mirrorchyan_rid: <rid>
    upload_token: ${{ secrets.MirrorChyanUploadToken }}
```

要点：

- **`os`/`arch` 要么都写、要么都不写。** MAA 是按 `os`/`arch` 矩阵分别上传 `MAA` 这个资源的（§10.1），
  所以它请求时必带这两个参数；`MaaResource` 不分区就一个都不带。我们只有 win-x64，
  **无论选哪种，上传侧和请求侧必须一致**，否则就是 404/8001。
- **地区包要另外的 `res_id`**，且得像 MAA 那样从另一个仓库/另一条 workflow 单独上传（§10.1）。
- **发布说明**可以另用 `MirrorChyan/release-note-action@v1` 单独推，不必混在包上传里。
- **Token 只进 secret**：`gh secret set MirrorChyanUploadToken`。GitHub 会把它显示成全大写
  `MIRRORCHYANUPLOADTOKEN`，属正常现象，引用时不区分大小写。**不得写进仓库文件。**
- **每个版本都要有一个整包**：Mirror酱的增量是它自己按相邻版本 diff 出来的，
  只有"这个版本被上传过"，玩家从那个版本上来时才拿得到增量包。我们分片发布时
  `-ManualInstallZip` 是可选的，所以需要一条"把分片重新组装成整包再上传"的路径
  （`prepare` 签名前本来就会把所有分片重组回一棵树，复用这一步即可）。
  这份包**不进签名清单**，与本次发布的绑定靠同名 `.report.json` 核对（同 `-ManualInstallZip` 的做法）。
- **顺序**：在 `Publish-ResourceUpdate.ps1` 推进 `updates/stable.json` **之后**再上传 Mirror酱，
  让镜像永远不领先于权威清单。客户端本来就"只认清单里的版本"，即便镜像先行也会被忽略，但这层保险值得留着。
- **体积**：整包约 **950 MB**（`out/maps-2026.9.25.2/program/*.zip` 七个分片合计 949.1 MB，
  展开后 1,412 个文件、1.51 GiB），低于 GitHub 的 2 GiB 限制，也低于 `ProgramPackage.Validate` 的 2 GiB 上限。
  但**每个版本往 Mirror酱 传接近 1 GB**，这是接入前最该跟技术支持确认的一项
  （他们的增量只影响玩家下载量，不影响我们上传量）。

## 8. 分阶段实施计划

| 阶段 | 内容 | 依赖 | 玩家可感知的收益 |
| --- | --- | --- | --- |
| **P0** | 申请 `res_id`；跟技术支持确认上传形态（单包/多包、是否带 `os`/`arch`、整包体积、`sp_id` 是否可用、`custom_data` 上限） | 联系 Mirror酱 | — |
| **P1** | 免费通知 + 引导：`CheckAsync` 并发调 API；清单失败时也能显示"有新版本 vX"并给出国内可下载入口；设置页加 Mirror酱 跳转链接 | P0 的 `res_id` | **没 VPN、没 CDK 的玩家第一次能知道有新版，并拿到能打开的下载入口** |
| **P2** | 清单国内镜像（§6.1 方案一）：`StableUri` 改有序列表 | 选定镜像宿主 | 检查更新在国内可用，不再依赖代理 |
| **P2 ✅** | **已完成（2026-09-25）**：清单多来源（GitHub → Gitee 镜像）、发布脚本推送镜像、5 条回归测试 | Gitee 仓库 + 令牌 | **检查更新在国内可用，不再依赖代理**（仍受"下载走 GitHub"限制） |
| **P3** | CDK 路径：设置项 + DPAPI 存储 + 错误码处理 + 镜像下载通道 + 逐文件校验 + 回退分片 | P0、P2 | 付费玩家一键快速更新，且全程受签名清单保护 |
| **P4**（可选） | 地区包的独立 `res_id`；日活/来源统计的 `source` 参数铺开 | P3 | 地区包也走国内 CDN |

## 9. 验收清单（照官方 Skill 的清单，按我们自己的安全模型改写）

- [ ] 不填 CDK：`code == 0`，有 `version_name`，无 `url`，程序正常回退到 GitHub 分片。
- [ ] 填入错误 CDK：`7002`，界面明确提示，**不崩溃、不影响其他更新源**。
- [ ] 断网或超时：不阻塞启动、不崩溃，超时与现有下载器一致。
- [ ] `8001`：提示"当前平台/通道暂无可用更新"，回退 GitHub，不当成严重错误弹窗；日志记录 `os`/`arch`/`channel`。
- [ ] `update_type == "full"`：先等待并重问一次；仍为全量则**先弹确认**再下载整包（约 950 MB）。
- [ ] `update_type == "incremental"`：增量包解压后仍按签名清单逐文件校验，装配结果与全量包逐个文件等价。
- [ ] 已是最新：不提示更新；`release_note == "placeholder"` 时**不显示**公告。
- [ ] `version_name` 与签名清单 `app.version` 不一致（比如镜像领先于清单）：**忽略这条通道**，不下载。
- [ ] 镜像包被篡改（改一个文件、加一个清单外的文件、删一个文件）：`VerifyDirectoryAsync` 全部拒绝，
      **不安装、不覆盖**运行中的程序。
- [ ] 在日志、配置导出、崩溃报告里 grep CDK，确认没有明文。
- [ ] `user_agent` 与跳转链接的 `source` 已按约定设置。
- [ ] 官方 Skill 的 `changes.json` 语义（`deleted` / `deleted_dir`）**不需要实现**——
      用"按清单重建 + 目录全量校验"覆盖；补一条测试证明"清单外的残留文件会被拒绝"。

## 10. 参考实现：MAA 是怎么接的（源码实证）

MAA 是目前接入 Mirror酱最成熟的项目，直接回答"增量更新 + 程序更新/资源更新两条线"是怎么落地的。
来源 `MaaAssistantArknights/MaaAssistantArknights` @ `dev-v2`，关键文件：
`Constants/MaaUrls.cs`、`ViewModels/Dialogs/VersionUpdateDialogViewModel.cs`、`Models/ResourceUpdater.cs`、
`Services/PendingUpdateApplier.cs`、`Configuration/Global/Update.cs`、
`.github/workflows/release-package-distribution.yml`。

### 10.1 两个 `res_id`，两条互不相干的链路

| 链路 | `res_id` | URL | `current_version` 格式 | 平台参数 |
| --- | --- | --- | --- | --- |
| **程序更新** | `MAA` | `/api/resources/MAA/latest` | SemVer `v5.13.1` | `os=win&arch=x64\|arm64&channel=stable\|beta\|alpha` |
| **资源更新** | `MaaResource` | `/api/resources/MaaResource/latest` | **时间戳** `yyyy-MM-dd+HH:mm:ss.fff` | 一个都不传 |

- 两条流**各自独立上传**：`MAA` 由 `release-package-distribution.yml` 的矩阵（win/x64、win/arm64、macos/arm64、
  macos/x64）上传，所以客户端**必须**带 `os`/`arch`；`MaaResource` 由**另一个仓库** `MaaAssistantArknights/MaaRelease`
  触发上传（`gh workflow --repo .../MaaRelease run mirrorchyan_alpha.yml`）。这印证了 §6.2 的判断：
  **一条更新流一个 `res_id`**，而且"按平台上传的要带 `os`/`arch`，不按平台的绝对不能带"。
- **`current_version` 没有格式要求**，Mirror酱只做字符串匹配（命中才给增量）。MAA 的资源流甚至用的是时间戳，
  所以我们用四段数字版本完全没问题——只要与上传时的 `version_name` 一致。
- 程序更新还额外发了两个参数：`sp_id`（机器码的稳定哈希，官方文档里没有这个参数）和 `user_agent=MaaWpfGui`。

### 10.2 增量的来源：MAA 自建 OTA 与 MirrorChyan 是两回事

`UpdateSource` 只有两个值：`GitHub` 和 `MirrorChyan`（默认 GitHub）。程序更新的海外源走 MAA 自己的
`api.maa.plus` + 自建 CDN（`s3.maa-org.net`、`maa-ota.annangela.cn`、腾讯 COS），它的 OTA 包名精确到
"从哪个版本到哪个版本"：`MAAComponent-OTA-vFROM_vTO-win-ARCH.zip`；找不到就退回完整包并提示"无增量包"。

MirrorChyan 这条线靠 `update_type`：

```csharp
// TryWaitForMirrorChyanOtaAsync
if (data["data"]?["update_type"]?.ToObject<string>() != "full") return data;   // 增量已就绪
await Task.Delay(10000);                                                       // OTA 可能正在打
var retryData = await FetchMirrorChyanJsonAsync(url);                          // 重问一次
if (retryData?["data"]?["update_type"] != "full") return retryData;             // 拿到增量
_requiresFullPackageConfirmation = true;                                       // 只能全量 → 先让用户确认
```

> 注释原文：「MirrorChyan 在收到首个请求后会开始打包 OTA，打包过程中返回完整包。等待 10s 后重试，通常此时 OTA 包已就绪；
> 若重试后仍为完整包则走完整包更新途径。」

### 10.3 MAA 怎么应用增量包

- **是不是增量，看包内有没有 `changes.json`（旧格式 `removelist.txt`），不看文件名**（`HasOtaMetadata`）。
- `changes.json` 只读 **`deleted`** 一个字段当 `removeList`；`added`/`modified`/`added_dir` **不读**，
  `deleted_dir` 更是**完全没读**（结尾带 `/` 的条目还会被 `IsDirectoryRemovalEntry` 滤掉）。
- 应用分两档（`ShouldDelegatePendingOtaApply`）：**只动资源文件 → 进程内直接覆盖 + 删除**；
  **碰到"运行时敏感"路径（DLL 等）→ 交给独立的原生 `MaaUpdater.exe`**，命令行传
  `{ "packageType": "full|ota", "removeList": [...], "moveList": [...] }`，由它改名运行中的文件、写入新文件、
  下次启动再删旧文件。
- **完整性存疑时禁用增量，只允许完整包**——代码注释的理由：*"资源损坏或上次更新失败标志存在期间，
  OTA 增量只含差异文件，无法修复残留的不一致文件"*。
- 完整包的应用是"**清空安装目录 + 白名单保留**"（保留 `achievement`/`cache`/`config`/`data`/`debug`/`MAA.Updater.exe`，
  以及嵌套保留 `Res\Backgrounds\Wallpapers`），所以下载前必须弹确认（`_requiresFullPackageConfirmation`）。
- 拖入本地包时用两个正则校验架构与版本方向：OTA `^MAAComponent-OTA-(from)_(to)-win-(arch)\.zip$`、
  完整包 `^MAA-(version)-win-(arch)\.zip$`。
- **资源**那条线反而没有增量语义：`DirectoryMerge` 是**递归覆盖合并，不处理任何删除**，
  只把 `version.json` 放在最后复制当作"提交点"（中途失败则本地版本号仍是旧值，下次重试）。

### 10.4 该借的和不该借的

**该借的**：`update_type` 判定 + "首次请求通常是 full"的重试；退化成全量前先征得用户同意；
"增量不用于修复损坏安装"这条纪律；就地下次启动才应用的延迟提交模型；§10.5 的整套 CDK 产品化细节。

**不该借的**：MAA 对下载来的包**不做任何密码学校验**（无签名、无逐文件哈希），信任完全来自 HTTPS 与来源；
它的完整包应用是"清空目录再铺"，删错了就没有退路。**我们的方案比它强，不要为了对齐 MAA 降低自己**——
在 MAA 那里 MirrorChyan 是"可信来源"，在我们这里它只能是"**不可信传输层**"（§3）。

还有一条旁证：MAA 自己同时维护 `api.maa.plus`、`api2.maa.plus`、S3、腾讯 COS、`maa-ota.annangela.cn`
**五个国内可达的源**，并没有把国内可达性押在 Mirror酱一家身上。这正说明 §6.1 那件事必须自己做。

### 10.5 CDK 的产品化细节（可以直接照抄）

| 做法 | 出处 |
| --- | --- |
| CDK 加密后落盘，配置里不是明文 | `MirrorChyanCdk` → `SimpleEncryptionHelper.Encrypt` 存入 `ConfigFactory.Root.Update` |
| 记住 `cdk_expired_time`，界面显示"剩余 N 天"，≤7 天变色告警 | `MirrorChyanCdkRemainingText` / `MirrorChyanCdkRemainingBrush` |
| 日志里 CDK 掩码，且只记 URL 的 path（`uriPartial: UriPartial.Path`）不记 query | `CheckUpdateByMirrorChyan` |
| 每个错误码一句玩家看得懂的提示；1001/8001–8004 直接显示服务端 `msg` | `HandleMirrorChyanErrorCode` |
| 更新源做成用户可见的开关（GitHub / MirrorChyan，默认 GitHub）；选了 MirrorChyan 却没填 CDK 时明确提示 | `UpdateSource`、`MirrorChyanSelectedButNoCdk` |
| **没填 CDK 但确实有新版本时，照样提示"有新版本 + 可填 CDK 加速"，再回退 GitHub** | `CheckUpdateRetT.NoMirrorChyanCdk` |
| 跳转链接带 `source=`：`mirrorchyan.com?source=maawpfgui-settings`、`mirrorchyan.com/zh/projects?rid=MAA&source=maawpfgui-manualupdate` | `MaaUrls` |
| 发布说明单独用 `MirrorChyan/release-note-action@v1` 推送 | `mirrorchyan_release_note.yml` |
| 给"首次使用 Mirror酱""CDK 填错"发成就（可选的小彩蛋） | `AchievementIds.MirrorChyanFirstUse` / `MirrorChyanCdkError` |

`sp_id` 不在官方文档的参数表里。要么按 MAA 的做法发（服务端显然接受），要么先不发——这一点值得在 §8 P0 里一并问清楚。

## 11. 参考

- Mirror酱接入文档：<https://github.com/MirrorChyan/docs>（[README](https://github.com/MirrorChyan/docs)、
  `ErrorCode.md`、`Incremental.md`、`FAQ.md`、`.agents/skills/mirrorchyan-integration/SKILL.md`）
- 上传 action：<https://github.com/MirrorChyan/uploading-action>
- 第三方接入方对计费模型的说明：<https://github.com/moesnow/March7thAssistant>（`assets/docs/FAQ.md`）
- 参考实现 MAA（`dev-v2`）：`src/MaaWpfGui/Constants/MaaUrls.cs`、
  `ViewModels/Dialogs/VersionUpdateDialogViewModel.cs`、`Models/ResourceUpdater.cs`、
  `Services/PendingUpdateApplier.cs`、`.github/workflows/release-package-distribution.yml`
  （本次为只读调研下载的副本在 `out/maa-study/`，`out/` 已在 `.gitignore` 内）
- 本项目相关：`Docs/ResourceUpdates.md`、`Docs/ProgramUpdates.md`、`updates/README.md`
