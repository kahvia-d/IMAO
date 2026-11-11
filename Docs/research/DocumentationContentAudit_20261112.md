# 文档内容时效核对（2026-11-12）

用户在目录整理后要求再次仔细检查正文，并授权依据历史提交与当前仓库更新过期内容。本轮修订 35 份已有文档，重点是仍被当作操作入口的指南、设计说明、OpenWork 与计划状态。历史发布说明和实验数值保留原时间范围；未将“代码存在”当成“已实机验收”。

## 已修正的主要问题

| 原描述或风险 | 修订与证据 |
|---|---|
| 构建必须先生成旧基础 IMF/IMX | 基础图集已退役；当前使用地区包及共享词表。依据 `scripts/Test-BuildPrerequisites.ps1`、`Build-IMao.ps1`、`Build-FeatureBinary.ps1` 与 `134aff1`；构建指南不再指导常规克隆重建旧 XML |
| 旧 Paddle 取景、回归器被称为当前生产链 | 当前为自训 YOLO；旧 `IMaoCoordinateRegression` 仍显式加载 PP-OCR。旧离线工具的 `production` profile 是冻结实验名，不是现行模型；采集校验不再依赖已清理的旧环境 |
| flight 权重与旧摘要仍称当前模型 | `3c5d8fd`、`8ba9eb0` 已更新几何增强模型及采样/符号证据。对当前 `.pt`、ONNX、元数据实际核验摘要，补齐恢复材料入口；旧权重作为祖先保留 |
| 旧四包注册表、旧分层数量与全部关闭的新区 | 当前 14 地区包；分层清单 63 图 / 98 层 / 169 瓦片引用；四场景为 approved 且 local-capture-trial。依据资源 manifest 和发布脚本，不冒充完整实机准入 |
| OCR 只能排序、所有定位都必须 RANSAC、两秒统一隐藏 | 按当前 `App.cpp`、`OcrCoordinateGate`、`CoordinateHandoff` 与 `a096381` / `7d5fe8c` 修订，保留场景信任、独立观测与上下文边界 |
| 程序未变化分片始终复制、分片未实施 | `46d158c` 已实现受限硬链接复用与复制回退，完整目标摘要仍校验；共享词表 `134aff1` / `69388d2` 已实施。实际更新节省量的旧待验项没有凭空关闭 |
| 本机旧 prepare/upload 命令被当正式发布流程 | 正式入口改为当前云构建、本地签名、审批和事务推进，明确先提交并 push；本地实验和历史候选记录分开 |
| Mirror酱失败自动换 GitHub、仅资源更新必须切源 | 按 `CheckMirrorAsync` 与当前源选择实现修订；商业/网络观察注明历史日期，未宣称本次重新核验外部服务 |
| 同步只做并集、永不取消点位 | 安装与商店材料按 `KuroProgressSyncService`、`MarkerCompletionStore::DesiredCompletion`、`KuroSyncSchedule` 修订为基线与完成/取消变更；本轮没有对账号执行同步，也未更新商店后台 |
| bundled 不可删除、交付树可手拷 JSON | 按 `UpdateService.RemoveAsync` 的真实副本处理说明删除边界，测试树交付改为批准的整树脚本与核验 |
| OpenWork 的 DComp、WGC 限速、ROI、共享词表、楼层身份等仍未实现 | 逐项写明已落地代码及日期，保留跨机器帧率、活动内存、独立精度和游戏现场的验收缺口 |
| 11 月计划仍让后来者重新执行已完成工作 | 104–107、内存与 11 月 11 日缩放/启动/窗口位置等补现行状态；历史任务书、对照实验和未验收范围保留 |

## 修订范围

- 构建、采集与研究恢复：`guides/Build_zh-Hans.md`、`CoordinateOfflineEvaluation_zh-Hans.md`、`CoordinateSampleCapture_zh-Hans.md`、`guides/ResearchArtifactRetention_zh-Hans.md`、`guides/CoordinateYoloResume_zh-Hans.md`、`research/CoordinateYoloCropRobustness_zh-Hans.md`。
- 地图与定位：`guides/KuroMapDataSource.md`、`guides/KuroSceneCalibrationSamples.md`、`guides/KuroTileFeaturePacks.md`、`guides/MarkerDetectabilityMaintenance.md`、`architecture/VisualLocalization_zh-Hans.md`、`architecture/LayeredMapFeaturePackPlan.md`、`architecture/MapRegionOnDemandHandoff.md`、`MapRegionRefactor-Handoff.md`。
- 同步：`KuroMapSyncInstall.md`、`guides/KuroMapSyncStoreListing.md`。
- 发布更新：`ProgramUpdates.md`、`ResourceUpdates.md`、`CloudRelease.md`、`architecture/ProgramShardIncrementalPlan.md`、`architecture/MirrorChyanIntegration.md`。
- 待办与计划：`OpenWork.md`，以及 `plans/` 中的 `2026-11-11-overlay-sizing.md`、`2026-11-11-startup-latency.md`、`2026-11-11-window-placement.md`、`MiniMapOverlayDesign_zh-Hans.md`、`MinimapMarkerRenderPerfPlan_zh-Hans.md`、`NoExtrapolationPlan_20261101.md`、`OcrAsyncFloorPlan_20261101.md`、`OcrDelayPlan_20261101.md`、`OldPcLocalizationPlan_20261031.md`、`PerformanceOptimizationPlan_zh-Hans.md`、`RuntimeMemoryPlan_20261020.md`、`StatefulDisplayPlan_20261101.md`、`VisibilityAndDeltaPlan_20261031.md`。

## 尚未替代的验证

本轮没有构建、训练、执行真实更新或操作玩家客户端。完整玩家启动耗时、4K 游戏交互、旧电脑表现、活动内存峰值、跨区域精度及尚未记录的实机验收继续保留。外部商店、镜像服务、下载可达性和收费政策未重新在线核验；既有相关调查均按历史范围阅读。

这次核对不是对每一条历史数字重新实验，也不证明所有旧待办均已解决。需要继续工作的具体事项仍以 [OpenWork](../OpenWork.md) 的带日期条目为入口。

## 本轮检查结果

文档整理核验脚本检查 592 处本地链接，未发现断链；原有 199 个文档及校验清单文件全部保留，索引覆盖完整。固定路径、图片、发布公告正文与校验清单未改变；程序源码没有差异，既有 PowerShell 改动仍仅为上一轮的注释路径。`git diff --check` 通过。提交与推送结果以 Git 历史及远端分支为准。
