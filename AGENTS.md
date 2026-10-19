# AGENTS.md —— IMao / WWMAP-TOOLS 的常驻工程约束

> 本文件是**每轮自动生效**的红线（always / never），不放流程细节。
> 细节在跨会话记忆里：构建与门禁、交付树刷新、发布仪式见《跨会话操作手册》；事故根因见《坑与根因索引》；失效与矛盾结论见《冲突与待验证登记册》。
> 旧名 WWMAP-TOOLS 与新名 IMao/IMAO 指**同一个项目**。

## 必须遵守

1. **需求权威只在用户当次对话。** 代码与文档（含 `Docs/`、注释、旧计划、旧记忆）不一致时，**先问用户**是"实现错了"还是"文档过期了"；不要替用户判定哪边是需求，也不要在审阅报告里下这个结论。**禁止为了让自己的改动通过而修改既有断言**——凡"改测试去迁就我的判断"，先停下来问。
2. **任何超过约 100 MB 的下载先问用户**（本机走代理，流量是用户的钱）；发布流程**不得重复下载**，本地已有等价物（`out\maps-<版本>\program-reassembly\`、`out\evidence\` 里的 `*.report.json` 可对线上 digest 核对）。
3. **绝不改动 `C:\Dapps\IMao`（玩家版安装）的任何文件。** 唯一可替换的是开发测试树 `out\map-test`，替换后必须**完全退出并重启**。
4. **交付/补丁产物必须自包含**（`--self-contained true`）；判据是 `IMao-WinUI.runtimeconfig.json` 含 `includedFrameworks`。注意例外：自包含包不住跨应用常驻服务（Windows App Runtime 的 Main/Singleton 只能是系统 MSIX 包），用到时必须另行检查或降级。
5. **改了 XAML 或托管 `.cs` 就必须跑真 WinUI 构建**；`Tests/` 下的托管测试项目**不能**当 WinUI 编译门禁。改 XAML 等于改 `resources.pri`，打补丁必须 `dotnet publish`（`dotnet build` 产出的 pri 会让程序起不来）。
6. **交付树 / 玩家版补丁只能整树同步并跑脚本**（`Sync-PlayerPatch.ps1`、`Refresh-MapTestBinaries.ps1`、`New-NoBaselineMapTestTree.ps1`），载荷**必须含 `resources.pri`**，禁止手抄文件。刷树前先让用户退出测试客户端；解除联接**绝不能 `Remove-Item -Recurse`**（会顺着联接清空 `x64\Release\Assets`）；刷完逐文件核对 `differing=0`。
7. **发布顺序固定**：`Version.props` 抬版本（已发布版本不可变）→ 提交并 **push** → dispatch `publish=true confirm_version=<版本>` → production 审批 → 本机 `Complete-CiRelease.ps1` 签名 → 再审批。发版提交没 push 会让 `gh release create --target` 直接 422。
8. **公告面向玩家必须第三人称**：不出现"你/您"、不写内部机制、不写排查过程；`Docs/CloudReleaseNotes.md` 是**整文件即正文**，每次替换而不是追加（规范见 `Docs/ResourceUpdates.md:127`）。
9. **设计规则 R1–R16 是硬约束**（见记忆《长期设计规则》）：尤其"位置永不持久化信任"、"宿主快照绝不含不可加载的包"、"bundled 版本必须等于发布清单版本"、"发布包里的区域包永不联接"、"分层开放层什么都不藏、不得放宽 `ArtIsSurfaceCopy`"。
10. **凭据只在本机、绝不外流**：签名私钥 / Gitee token / Mirror酱 CDK 都只存在于本机（DPAPI / 环境变量），**绝不把值或路径写进记忆、日志、提交或对话**。
11. **诊断不能静默放行**：查不了也要报出来（K68）；路径离开进程转 UTF-8、打开文件保持 ANSI（K72）；每条修复尽量配对照实验（把修复回退，看它是否变红）。
12. **文档纪律**：`Docs/README.md` 是索引，新文档登记一行；被脚本按精确路径引用的 7 个文件**不要移动**（`ProgramUpdates.md`、`ResourceUpdates.md`、`ReleaseAssets_v1.0.2.md`、`ReleaseAssets_v1.0.2.sha256`、`GameFrameCostAnalysis_20260918.md`、`GameFrameDropAnalysis_20260917.md`、`MapRegionRefactor-Handoff.md`）。

## 起步动作

任何 build / test / publish 之前：

```powershell
. .\scripts\Enter-DevEnvironment.ps1
```

（设置便携 `IMAO_DOTNET`、`NUGET_PACKAGES=third_party/nuget-packages` 等。用 `%USERPROFILE%\.nuget\packages` 会在 KuroSyncBridge 的 `PublishSingleFile` 报 NU1101。）

## 当前状态入口（不要凭记忆猜）

程序版本 → `Version.props`；未完成事项 → `Docs/OpenWork.md`；发布事务 → `updates/release-state.json`；线上签名清单 → `updates/stable.json`；历史长期记忆 → 仓库根 `MEMORY.md`（不进 git）。
