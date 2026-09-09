# 资源更新系统验证记录（2026-09-09）

当前推荐交付为 **2026.9.9.2**：修复首次实机试用发现的离线导入文件选择器异常，并补充实际交付 DLL 的系统对话框测试。下载位置、哈希及补丁验收见 [离线导入入口修复记录](ResourcePackagePickerFix-20260909.md)。下列 2026.9.9.1 为首版历史记录，其完整程序包已被新补丁替代，签名离线资源包仍可继续使用。

程序版本为 `2026.9.9.1`，基础资源标识为 `map-baseline-1`。本轮已实现客户端、原生资源快照、签名协议、发布工具和本地交付包。维护及使用方法见 [ResourceUpdates.md](ResourceUpdates.md)。

本轮修改尚未提交。构建报告中的 `sourceCommit=13be5284f538f960b7a0c6e3cc0097aea0f0b72a` 是工作区基准提交，**不表示该提交已经包含这些修改**；报告同时记录 `sourceDirty=true` 和实际暂存时的 `sourceTreeSha256`。下列 ZIP 是已验证的本地验收交付，正式发布脚本会拒绝脏工作区构建。未创建远端发行、推送源码或推进稳定清单。

## 已实现行为

- 启动后台检查最多每 24 小时一次；可关闭、手动检查；失败不误报最新。程序更新打开本仓库发行页，地图资源需用户点击安装。
- 单一 `ResourceSnapshot` 固定本次 WinUI 与 CoreHost 的点位、分类、图标、攻略、校准和特征包组合。更新仅写新的版本目录，完整退出并再次启动后启用。
- 已安装快照的筛选目录完整采用外部资料，允许修改现有名称及移除旧分类；本地筛选、完成记录和路线的持久化位置不变。
- ECDSA P-256/SHA-256 签名、清单序号防降级、包和逐文件哈希、路径及可执行文件检查、流式传输、超时与取消、跨进程安装锁、失败恢复及整套回退。
- 离线集合包验签并验证全部 ZIP，包括可复用包；离线验收确认零网络请求。首版保留已安装历史目录，不自动回收包，避免删除其他实例依赖的版本。
- 发布入口分离本地准备和远端操作；附件公开校验成功后才推进 `updates/stable.json`。清单签名和资源路径规则由客户端与发布工具共享。

## 验证结果

| 检查 | 结果 | 证据目录或文件 |
|---|---|---|
| 实际 WinUI 自包含发布 | 通过；现有 StringItems 空值分析警告仍保留 | `out/update-build/publish-verified.log` |
| 更新核心故障与事务测试 | 51 项通过 | `out/update-runtime-verified/resource-updates/results.json` |
| 发布器测试 | 16 项通过，包含两轮差异打包 | `out/resource-updates-tests/publisher-tests.log` |
| PS5/PS7 一致性、重复暂存及旧文件拒绝 | 4 项通过，旧文件原样保留 | `out/staging-canonical-regression-1/test-report.json` |
| 发布清单不可变性及兼容条目保留 | 14 项通过 | `out/catalog-transition-regression-4/test-report.json` |
| 托管运行时及真实 CoreHost IPC | 672 项 PASS | `out/update-runtime-verified/managed-tests.log` |
| 设置页及现有窗口回归 | 96 项断言通过；检查 1120/800 尺寸截图 | `out/main-window-runtime/ui-tests.log` |
| 原生快照检查 | 41 项通过 | `out/resource-snapshot-native/snapshot-tests.log` |
| 现有原生优化、标记、路线、保存失败 | 通过 | `out/resource-snapshot-native/native-validation-report.json` |
| 可用地图回放基线 | 五份清单、56 个样本通过 | `out/resource-snapshot-native/visual-summary.json` |
| 首次资源集合包复用 | 实际 CoreHost 启动确认与回退通过 | `out/resource-real-e2e-reuse-2/real-e2e.json` |
| 四包实际解压安装 | 实际 CoreHost 启动确认与回退通过 | `out/resource-real-e2e-extraction-rename-fix/real-e2e.json` |
| 独立完整程序验包 | 资源预检、最小 PATH 下 IPC、正常退出、包内容不变 | `out/update-delivery-ready-2026.9.9.1/verification/package-probe.json` |
| 最终交付程序与离线包配对 | 实际导入、启用确认和回退全部通过 | `out/resource-real-e2e-ready-delivery/real-e2e.json` |

真实端到端测试采用独立测试用户目录，验证“安装后当前快照不变 → 重启选择候选 → 收到实际 CoreHost 的相同快照 ID 和就绪状态 → 确认成功 → 重启回退”。用户数据哨兵逐字节保持不变。完整解压测试使用独立的 `2026.9.9.2` QA 集合包强制写入四个新版本目录；该 QA 包不是正式更新入口。

签名测试覆盖错误/未知/测试密钥、篡改内容、旧序号、重复版本内容、哈希错误、越界 ZIP、重复文件、链接、脚本、取消、空间不足、网络错误、差异下载、已有包复用和异常退出恢复。原生预检补充基础 IMF/IMX 哈希、八场景点位、分类/图标/攻略引用、所有 JSON 解析、包依赖及未开放地区准入。

PS5 实际暂存验证发现并修复了数组序列化包装差异，改为保留点位 JSON 原始数值表示；同时补全既有 902/909/910 对应的空运行时场景映射。核对八场景 **23,803 个点位**的 ID、坐标、state ID 和描述均保持一致，场景关闭状态不变。

总回归还复现了 Windows 对关闭文件后的原子替换/目录提升临时拒绝访问。已增加统一的可取消有限重试，仅重试 Windows 错误码 5/32/33；持续拒绝仍失败并保留旧状态，不删除目标解除占用。六个实际持有文件句柄的测试验证临时占用、持续占用及取消；相同回归环境和真实四包安装均再次通过。未将此问题归因到未经确认的具体软件。

最终配对检查捕获 PS5/PS7 对 `manifest.json` 的格式差异。客户端正确拒绝同 ID/版本但字节不同的内置数据；已将暂存统一委托给 Windows PowerShell 5.1，验证含空格目录、相对路径及全部 560 个地图数据文件的哈希一致性，再对下列实际交付程序与离线包完成整套端到端验收。早期中间程序包已标记不可分发，未更改签名离线包或放宽校验。

## 本地交付

完整程序：`out/update-delivery-ready-2026.9.9.1/IMao-v2026.9.9.1-windows-x64.zip`，526,288,946 字节。

SHA-256：`cd5facd876db49a9a4683696166999bfb5bb740faf8ab8fbb4e9ed335513c41e`。

签名离线资源包：`out/resource-release-2026.9.9.1-final/resources-2026.9.9.1-offline.zip`，165,390,306 字节。

SHA-256：`c409e287615d380909ad5883e26b1bdbb54a825194fbc295ed5e4607e767517e`。

生产公钥随程序提供。私钥仅位于维护者账户的 `%LOCALAPPDATA%/WWMAP-TOOLS-Publisher/release-signing-key.json`，以当前用户 DPAPI 加密，不包含在仓库或发行包中。

## 验证边界

- Windows 沙箱曾阻止本机命名管道连接。保留首次失败记录，经自动审批后在沙箱外使用独立测试目录重跑，实际通信及资源启用/回退已通过。
- 回放证明现有离线基线兼容；未进行新地区实机定位、实体手柄或管理员 GUI 手动验收。历史总清单缺 111 个输入，西部旧 smoke 清单缺 1 个输入；没有把这些缺失样本计作通过。
- 未修改场景 ID、上游 state ID 或点位 ID；未启用 BlackShores 或任何未经既有准入验证的地区。
- 远端草稿创建、附件公开下载和稳定入口推进尚未执行；线上发布幂等性与网络环境需在真实首次发布时完成验证。正式发布须从审阅后的干净源码提交重新构建。

协议参考：[GitHub Releases](https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases)、[.NET ECDsa.VerifyData](https://learn.microsoft.com/en-us/dotnet/api/system.security.cryptography.ecdsa.verifydata)。
