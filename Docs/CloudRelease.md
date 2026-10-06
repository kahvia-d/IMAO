# 云端构建、本地签名发布

生产私钥仅由本地已安装的可信 UpdatePublisher 使用 DPAPI CurrentUser 解密，在内存中签名并清零。Actions 不接收私钥，也不运行生产签名命令。客户端签名格式不变。

实施顺序：拆分准备和签名；加入双签名请求与产物验证；加入 Git 事务及原子 stable 推进；固化构建依赖和权限隔离的工作流；本地助手与恢复文档；测试、独立审查。

构建产物保持在云端。唯一往返数据是独立的小签名请求（展开不超过 16 MiB）以及签名响应（不超过 16 KiB）。请求包含原始 catalog 字节、审批 payload、所有附件的摘要、准备报告和前一 stable。审批签名同时绑定源码、版本、事务、预留 sequence、artifact ID/digest、安装 ZIP 及报告；客户端签名仍是 P-256/SHA-256/P1363。

正式发布必须显式 publish=true 和 confirm_version，production Environment 限制 main。构建默认仅预演，没有生产凭据。Gitee 和 Mirror酱使用独立 Environment，只在 GitHub stable 成功后运行。

发布锁由 Actions concurrency 和 main 的 release-state Git CAS 共同实现。锁内重读 stable/channel-state 后分配 sequence，已分配的数字永不复用。等待本地签名期间持久事务阻止下一正式发布。发布中断复用原始 request/response、draft ID 和相同附件；stable、channel-state、完成事务由一个 Git commit 推进。

阶段验收：临时密钥端到端；payload/安装 ZIP/分片/报告篡改拒绝；错误来源/版本/用途拒绝；过期基线拒绝；并发 CAS、丢失响应和幂等恢复；工作流权限静态检查；publish=false 全量云端预演通过后才能启用 production。

## 配置与使用

SDK 由 global.json 固定；.github/dependencies.lock.json 固定官方 Paddle/OpenCV 下载 URL、版本、完整下载摘要和 VGG 补丁摘要。只缓存经重新验 hash 的下载包及 NuGet 缓存，不缓存本机 CMakeCache 或编译目录。安装 ZIP、资源包、分片与签名请求 ZIP 使用固定顺序及 ZIP 时间戳；Publisher 固定 .NET 8.0.30 runtime 防止离线 ZIP 配方随 Runner runtime 改变，native 编译/链接使用 /Brepro，构建时间以源码提交时间为准。

管理员运行 `pwsh ./scripts/Configure-CiReleaseEnvironments.ps1 -MirrorChyanTokenFile <上传 Token 文件路径>`：production 仅 main、人工 reviewer 为 kahvia-d、允许该维护者确认自己的发布、禁止管理员绕过；gitee 与 mirrorchyan 独立 main Environment。Gitee Token 从本地已有文件迁移；Mirror酱必须提供上传 Token 的文件，不能用消费端 CDK。成功写入 Environment 后才删除旧仓库级 Mirror Secret。生产签名密钥完全不参与此配置。

1. 提交并审核本次源码，更新 Version.props 和 Docs/CloudReleaseNotes.md。先在 main 手动运行 **Cloud release build**，保持 `publish=false`。预演完成真实 native/managed 测试、ZIP、分片、资源包以及临时测试密钥的 finalize，不分配正式 sequence，也不更改 Release/stable/镜像。
2. 预演绿色后记录该 Run ID：`gh variable set CLOUD_RELEASE_REHEARSAL_RUN_ID --repo kahvia-d/IMAO --body <RunID>`。正式构建会验证它确为这个源码 SHA 的成功完整预演，且有预演证据。然后运行同一 workflow，显式 `publish=true`、`confirm_version=<Version.props>`。production 审批后生成小请求，Runner 结束。
3. 在可信 Windows 本机，从审核过的本地源码构建 UpdatePublisher；运行 `pwsh ./scripts/Complete-CiRelease.ps1 -RunId <正式构建RunID> -ConfirmVersion <版本> -ExpectedSourceCommit <40位SHA>`。核对展示的安装包摘要与请求后输入版本确认。只下载最多16 MiB请求，只上传不到16 KiB双签名响应。默认密钥仍位于 `%LOCALAPPDATA%/WWMAP-TOOLS-Publisher/release-signing-key.json`；不自动选择 cloud artifact 内的工具或公钥。
4. 再确认 production 发布审批。云端核验 artifact service digest、双签名、每个文件摘要、安装 ZIP 与分片的完整同树关系，然后复用/上传 draft、确认远程摘要与公开可达性、发布 immutable Release，最后一个 Git commit 同步 stable/channel-state/事务。Gitee、Mirror酱自动独立同步，随后清理已成功发布的大 artifact；小请求留30天。

## 恢复

构建失败重跑构建，不消耗 sequence。prepare 中断只重跑失败 Job（不要重跑已经冻结的 Build），相同 artifact 和已预留 sequence 自动恢复；已上传小请求按 ID/digest 复用。

正式发布和恢复的 confirm_version 对照已批准源码 SHA 中的 Version.props；等待期间 main 版本前进不会改变原事务。draft 额外附件或不同安装 ZIP 会在上传/公开前拒绝，未给安装包参数时自动选择已签名批准的唯一安装包。

本地小文件下载或解压若被中断，先删除助手工作目录中的不完整 `request.zip` / `request` 后重试；保留已生成的 `response.json`，不要重新签名。

本地 `response.json` 保存在 `%LOCALAPPDATA%/WWMAP-TOOLS-Publisher/ci/<RunID>/<RequestArtifactID>/`，重试助手会复用该响应，不能重新签名。发布中断重跑失败发布 Job 或重新 dispatch 相同字段；事务记录响应摘要、Release ID，已存在附件按远程 hash 复用，冲突停止且不会覆盖。已完成事务直接返回，不再建 Release 或分配 sequence。

等待签名可显式 `Publish-ResourceUpdate.ps1 -Phase abandon -Publish $true -ConfirmVersion <预留版本> -TransactionId <ID> -PreparedRoot <新的空工作目录>` 放弃；highestAllocatedSequence 不回退。发布已经开始只能恢复，不能自动超时释放。其他正式发布会等待/拒绝已有持久事务。

若 public Release 已完成而 stable 尚未推进，大 artifact 过期后可在云端从原 Release/保留旧附件恢复经过授权的字节；小请求另外永久附在 Release 的 `release-signing-request.zip` 中。若未公开且 artifact 过期，失败关闭：等待签名事务显式放弃后重新构建；已进入 draft 发布阶段需维护者恢复原有字节，不允许把新构建偷偷代入已签名请求。

镜像失败不回滚 GitHub，分别重跑 Gitee Job 或手动 dispatch mirrorchyan_release / mirrorchyan_release_note。镜像与正式发布共用并发组，取得锁后读实时 stable，旧 tag 自动跳过以防降级。

`release-authorization.json` 包含完整审批 payload、原始 inventory 字节与双签名，第三方可使用 `verify-install-authorization` 验证手动安装 ZIP。私钥隔离保证云端不能自行生成生产签名；本地审批仍然信任被审核的构建流程与源码，摘要绑定不能单独证明云端编译器没有遭到攻击。
