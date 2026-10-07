# 库街区点位与 PS 手柄测试交付

开发分支：`codex/kuro-points-ps-gamepad`，从 `ad74e0cc70c554eb5edf8b92e48c577cf13b9f79` 创建独立工作树。此阶段只更新本地 maptest；游戏内验收通过前不合并、不创建正式 Release、不推进 stable。测试版本暂沿用 `2026.10.6.1`，不表示该版本已发布这些修改。

## 点位与游戏内验收

完整同步九个区域和 541 个图标成功后才应用。梦枢·天落新增 31 个点位和两个分类；其他区域的点位内容无变化。逐点清单见 [KuroPointChanges_20261006.md](KuroPointChanges_20261006.md)。上游 ID、坐标转换、楼层信息及独立区域场景审批门槛均保留。

运行 `C:\Dcode\WWMAP-TOOLS\out\map-test\IMao-WinUI.exe`，重点检查梦枢·天落两类新增点位能否筛选和显示、位置是否对得上游戏，以及旧点位完成记录是否仍在。maptest 原来的地图包、无基础大图布局、场景校准和试验审批配置保留；新增点位不会自行批准场景。

`maptest-build-receipt.json` 记录本次源码提交、版本、快照 SHA-256、替换程序文件摘要及备份路径。必须以这份收据识别测试程序，不能只看版本号。

## 手柄操作

设置页可选择设备，并选择“自动 / Xbox / PlayStation”按键提示布局。原 Xbox 编号配置仍有效；自动选择优先 XInput，再选原生 PS。已经选中的设备断线后不会接管另一只；重连或切换设备后须先松开全部按键和摇杆。

原生 PS 设备名称会显示在列表中；手动选定设备后保存稳定设备路径的摘要。USB 和蓝牙可能提供不同路径，换连接方式后应刷新并重新选择。经 Steam Input 或 DS4Windows 转换为 XInput 的设备可手动指定 PlayStation 提示。

| Xbox 位置 | PS 位置 |
|---|---|
| A / B / X / Y | × / ○ / □ / △ |
| LB / RB、LT / RT | L1 / R1、L2 / R2 |
| LS / RS | L3 / R3 |
| Start / Back | Options / Share 或 Create |

大世界操作为 L1＋○ 完成点位、L1＋□ 打开攻略、L1＋Options 启停探索。攻略仍先被动显示，L3 才切换焦点；长按操作保留原时长和含义。界面和攻略使用同一选中输入源。组件加载失败会报告原因，Xbox 后端仍可用。

没有实体 PS 手柄，本次不能确认 USB 或蓝牙实机效果。玩家反馈应包含设备名称、连接方式、是否启用 Steam Input/DS4Windows、选中的设备与提示布局，以及相关 `input-state` / `guide-focus` 诊断；不要上传包含私人游戏进度的整个目录。

## SDL 依赖与打包

使用官方 SDL 3.4.18 源码 ZIP，下载 URL、大小、SHA-256 及补丁 SHA-256 固化在 `.github/dependencies.lock.json`。审查发现官方默认 PS 驱动在 USB 打开时可能自行发送灯光初始化，单设 enhanced-reports=0 不足以满足只读输入要求。因此 `patches/sdl3-3.4.18-input-only.patch` 只增加六处保护，禁止 PS4/PS5 增强报告关闭状态下的效果输出和蓝牙 tickle，并让 USB 默认值尊重关闭设置；基础按键与轴解析未修改。

本地和 CI 共用 `Initialize-GamepadDependency.ps1`，按锁定 MSVC 14.44 / SDK 26100 构建；应用以 override 优先级设置后台输入开启、增强报告关闭，不调用灯光、震动或触觉接口。`SDL3.dll`、`SDL3-LICENSE.txt`、`SDL3-BUILD.json` 一起进入开发输出和 publish。云端最终安装 ZIP 和分片增加 `Test-GamepadPackaging.ps1` 检查，逐个验证这三份文件及分片摘要。

本地验证开发输出和小 ZIP/分片夹具，包括拒绝被替换的 SDL DLL；约 1 GB 的正式安装包及真实分片留待验收后在云端全量预演，不在此次本地交付中上传大文件。

## 更新与恢复

`scripts/Update-MapTestBuild.ps1` 更新已有测试树，预备份受影响程序和 KuroMap/KuroMapIcons/Updates 及目录链接信息，只替换必要程序与点位资源，随后按实际保留文件生成快照并调用原生验证。替换前 unlink 硬链接，避免改动正式构建源；保留 SavedPoints、SavedRoutes、ProgramUpdates、地图包及模型。

**2026-10-07 交付纠正：**首次刷新遗漏了 memory 明确要求的 .NET 自包含构建，初始提交 be67735 的交付可启动性检查不足。`Build-IMao.ps1` 现在显式使用 `-r win-x64 --self-contained true`。刷新前、刷新后及构建输出都调用 `Assert-SelfContainedRuntime.ps1`：要求 runtimeconfig 为 `includedFrameworks` 且没有外部 `framework/frameworks`，检查 hostpolicy/hostfxr/coreclr/CoreLib、WindowsAppSDK bootstrap、Microsoft.UI.Xaml/Microsoft.WindowsAppRuntime/DWriteCore 和 deps 中 win-x64 runtime/native 文件。任何写入 x64/Release 的外壳构建都必须自包含，不能以“仅编译检查”为由省略。

`Test-SelfContainedDeployment.ps1` 验证正确构建，以及依赖外部 .NET、遗漏 WinUI SDK、遗漏 .NET 程序集三类错误输出被拒绝；刷新入口已验证在错误 runtimeconfig 下零备份、零复制。修复后重新验证更新失败回滚，再生成 clean SHA 的自包含构建用于交付。

**同日启动校验纠正：**旧生成器错误地把 formatVersion=2、bundled=false 的绝对路径候选写入 bundled-snapshot.json；原生候选校验通过并不能证明外壳接受内置清单。现按现有客户端协议写 formatVersion=1、bundled=true、相对路径的内置清单，format-2 候选仅保留在备份证据目录。部署额外执行 `ResourceBootstrapRuntime`，直接运行生产 `ResourceUpdateBootstrap.CreateSnapshots`、`ResourceSnapshotService.InitializeAsync`，再用实际选出的 CurrentPath 做 CoreHost 三项 ready 检查，且拒绝 LastFailure 回退。更新状态写入隔离测试目录，不修改玩家进度或真实更新状态。客户端协议与启动逻辑没有改动。

更新异常会逐项恢复资源、程序和原收据；若恢复某项失败，会继续恢复其他项并明确报告 `INCOMPLETE`，此时不要启动。备份中的 `original-links.json`、`binary-changes.json` 与 `backup` 可用于人工恢复。运行中断若没有成功收据，也应先按备份恢复，不继续使用混合测试树。

本次故障注入测试使用破损 CoreHost 可执行文件，使临时测试树在替换完成后的原生验证阶段失败；已确认快照、核心程序及资源目录所有权恢复。该测试未模拟恢复过程中发生文件锁或断电。

## 本地验证记录

本次通过：完整应用构建与六个 WinUI 页面导航检查、原生资源加载检查、九区域点位/图标检查、四独立区域发布门槛检查、完整 Test-Runtime（原生/托管/更新资源与程序分片回归）、修订 SDL DLL 的虚拟设备集成、真实 WinUI 攻略全套测试、PS/Xbox 输入服务、游戏焦点归还与跨进程焦点保护、maptest 故障回滚，以及 SDL 打包门槛的正常/篡改小包夹具。交付前完成独立审查并修复全部发现。

Windows 前台测试夹具改为明确显示自己的隐藏启动窗口、使用现有激活辅助方法，并等待真实按键采样再释放；断言仍要求真实操作系统前台归属。旧焦点归还夹具同时补齐“导航中”的状态，更新成现有生产的 passive → L3 聚焦 → B 返回 passive 行为，没有为了测试改变生产焦点流程。

日志保留在开发工作树 `out/task-kuro-ps/` 以及 `out/guide-window-runtime/`。正式云端大包预演和实体 PS USB/蓝牙测试尚未执行，不能由这些本地检查代替。

## 发布验收门槛

游戏测试通过反馈 → 修正与复测 → 合并 → 使用尚未发布的新版本与发布说明 → 在最终 main SHA 运行完整 `publish=false` 云端预演并记录 Run ID。旧版本或旧 SHA 的预演不能代替本次验收。

正式发布显式输入 `publish=true` 和完全匹配 Version.props 的 `confirm_version`，经 production 审批后仅交换小签名请求/响应。本地可信 UpdatePublisher 使用现有 DPAPI 私钥；云端只验证并上传原批次产物，继续使用原事务、原 sequence 和原签名响应恢复。GitHub、stable、Gitee、Mirror酱仍走既有独立发布入口与最小权限流程。
