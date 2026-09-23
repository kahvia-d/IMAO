# 文档索引

这个目录按"**现行指南 / 现行方案 / 历史记录**"三类组织。新文档请沿用这里的命名约定,并在本索引里登记一行。

| 命名 | 含义 |
|---|---|
| `<主题>_<YYYYMMDD>.md` | 当天的实现或排查记录(证据、决策、复现步骤),写完就不再更新 |
| `Release-<版本>.md` | 某次发行的说明 |
| `<主题>_zh-Hans.md` / `_en.md` | 面向使用者或维护者的参考文档,随代码更新 |
| `*Handoff*.md` / `*Plan*.md` | 阶段性方案与交接说明,完成后转为历史记录 |

> **不要移动这几个文件**:`ProgramUpdates.md`、`ResourceUpdates.md`、`ReleaseAssets_v1.0.2.md`、
> `ReleaseAssets_v1.0.2.sha256`、`GameFrameCostAnalysis_20260918.md`、`GameFrameDropAnalysis_20260917.md`、
> `MapRegionRefactor-Handoff.md`——脚本按精确路径引用它们(`New-ProgramReleasePackage.ps1` 会把前四个打进发行包,
> `Measure-WorkIsolation.ps1` / `OverlayTrace.Common.ps1` / `New-NoBaselineMapTestTree.ps1` 在注释里指向后三个)。

其他相关位置:仓库根的 `MEMORY.md`(跨对话长期记忆,**不进 git**)；`out/evidence/`(发布产物的小证据归档,
由 `scripts/Compact-ReleaseArtifacts.ps1` 生成,含 `archive-manifest.json`)；`updates/stable.json`(线上签名清单)。

## 待办与未完成

| 文档 | 内容 |
|---|---|
| [`OpenWork.md`](OpenWork.md) | 跨领域未完成项汇总（帧率、识别与资源、工程健壮性、更新系统、代码 TODO），带优先级与出处；每周或每次发布前过一遍 |

## 构建与编译

| 文档 | 内容 |
|---|---|
| [`Build_zh-Hans.md`](Build_zh-Hans.md) | 可复现构建（Windows x64） |
| [`Compile_zh-Hans.md`](Compile_zh-Hans.md) | 编译项目 CMake编译 |
| [`Compile_en.md`](Compile_en.md) | Compile the Project with CMake |

## 程序与资源更新（现行）

| 文档 | 内容 |
|---|---|
| [`ProgramUpdates.md`](ProgramUpdates.md) | 程序自更新 |
| [`ResourceUpdates.md`](ResourceUpdates.md) | 程序与地图资源更新 |
| [`ResourceUpdateValidation.md`](ResourceUpdateValidation.md) | 资源更新系统验证记录（2026-09-09） |
| [`ProgramShardIncrementalPlan.md`](ProgramShardIncrementalPlan.md) | 程序本体分片增量更新方案（已评审） |
| [`MapRegionOnDemandHandoff.md`](MapRegionOnDemandHandoff.md) | 区域按需下载 / 删除 · 设计说明（续篇） |
| [`MapRegionRefactor-Handoff.md`](MapRegionRefactor-Handoff.md) | 地图资源分区域重构 · 交接说明 |

## 地图数据、定位与校准

| 文档 | 内容 |
|---|---|
| [`KuroMapDataSource.md`](KuroMapDataSource.md) | 库街区地图数据同步 |
| [`KuroSceneCalibrationSamples.md`](KuroSceneCalibrationSamples.md) | 新地区四点校准样本 |
| [`KuroTileFeaturePacks.md`](KuroTileFeaturePacks.md) | 库街区瓦片特征包 |
| [`VisualLocalization_zh-Hans.md`](VisualLocalization_zh-Hans.md) | 小地图视觉定位 |
| [`PerformanceOptimizationPlan_zh-Hans.md`](PerformanceOptimizationPlan_zh-Hans.md) | 启动加载与坐标识别优化实施计划 |
| [`MapRecognitionAudit_20260906.md`](MapRecognitionAudit_20260906.md) | 2026-09-06 地图识别根因审计 |
| [`MinimapRetrievalAudit_20260907.md`](MinimapRetrievalAudit_20260907.md) | 小地图跨区域定位排查（2026-09-07 晚间） |
| [`MinimapRecoveryFix_20260907.md`](MinimapRecoveryFix_20260907.md) | 小地图跨区域恢复修复（2026-09-07） |
| [`SparseMinimapFusion_20260909.md`](SparseMinimapFusion_20260909.md) | 稀疏小地图与辅助定位整合（2026-09-09） |
| [`TethysCalibration_20260909.md`](TethysCalibration_20260909.md) | 泰缇斯四点校准与资源隔离 |
| [`RoyMaps_20260909.md`](RoyMaps_20260909.md) | 罗伊冰原地图补充 |
| [`CandidatePackRedundancy.md`](CandidatePackRedundancy.md) | 候选包是否已被梦州区域包取代 · 实测 |
| [`DreamzhouCandidateFeaturePack.md`](DreamzhouCandidateFeaturePack.md) | Dreamzhou curated-location candidates（已退役） |
| [`ResourcePackagePickerFix-20260909.md`](ResourcePackagePickerFix-20260909.md) | 离线导入入口修复记录（2026-09-09） |

## 界面、手柄与覆盖层

| 文档 | 内容 |
|---|---|
| [`MapTools_20260908.md`](MapTools_20260908.md) | 地图工具台与附近点位操作 |
| [`AutoRoutePlanningTests_20260908.md`](AutoRoutePlanningTests_20260908.md) | 自动路线规划测试命令 |
| [`RouteUsability_20260908.md`](RouteUsability_20260908.md) | 路线退出、删除与快捷键指南 |
| [`MarkerGuideLayout_20260908.md`](MarkerGuideLayout_20260908.md) | 攻略白边、默认位置与图片翻页 |
| [`MarkerGuideWindow_20260908.md`](MarkerGuideWindow_20260908.md) | 攻略浮窗开关与完成交互 |
| [`GamepadCursor_20260908.md`](GamepadCursor_20260908.md) | 大地图手柄光标点位修复 |
| [`ImageAnchoredOverlay_20260908.md`](ImageAnchoredOverlay_20260908.md) | 地形图像跟随与小地图浮动修复（2026-09-08） |
| [`OverlayContinuityFix_20260908.md`](OverlayContinuityFix_20260908.md) | 小地图闪烁与大地图快拖错位（2026-09-08） |

## 库街区进度同步扩展

| 文档 | 内容 |
|---|---|
| [`KuroMapSyncInstall.md`](KuroMapSyncInstall.md) | 库街区进度同步扩展：商店版（Edge，已上架）与手动安装（Chrome） |
| [`KuroMapSyncPrivacy.md`](KuroMapSyncPrivacy.md) | IMao 库街区进度同步扩展 · 隐私说明 |
| [`KuroMapSyncStoreListing.md`](KuroMapSyncStoreListing.md) | Edge 插件上架材料与上架后留存（Partner Center 填报用） |

## 审计、性能与重构

| 文档 | 内容 |
|---|---|
| [`ProjectAudit_20260907.md`](ProjectAudit_20260907.md) | 全项目审计与基础修复（2026-09-07） |
| [`Refactor_20260907.md`](Refactor_20260907.md) | 核心重构记录（2026-09-07） |
| [`GameFrameCostAnalysis_20260918.md`](GameFrameCostAnalysis_20260918.md) | 工具对游戏帧率的影响：玩家现场现象与成因分析 |
| [`GameFrameDropAnalysis_20260917.md`](GameFrameDropAnalysis_20260917.md) | 2026-09-17 游戏掉帧：工具侧开销分析与改动 |
| [`MinimapMarkerRenderPerfPlan_zh-Hans.md`](MinimapMarkerRenderPerfPlan_zh-Hans.md) | 小地图标记渲染掉帧优化 · 任务书与进度台账（现行） |
| [`MiniMapOverlayDesign_zh-Hans.md`](MiniMapOverlayDesign_zh-Hans.md) | 小地图局部覆盖层 · 正式设计（待评审） |

## 发行说明与发行资源

| 文档 | 内容 |
|---|---|
| [`Release-2026.9.9.3.md`](Release-2026.9.9.3.md) | WWMAP-TOOLS 2026.9.9.3 发行候选 |
| [`Release-2026.9.18.1.md`](Release-2026.9.18.1.md) | 2026.9.18.1 |
| [`Release-2026.9.19.1.md`](Release-2026.9.19.1.md) | 2026.9.19.1 |
| [`Release-2026.9.19.2.md`](Release-2026.9.19.2.md) | 2026.9.19.2 |
| [`Release-2026.9.22.1.md`](Release-2026.9.22.1.md) | 2026.9.22.1 |
| [`Release-2026.9.23.1.md`](Release-2026.9.23.1.md) | 2026.9.23.1 |
| [`ReleaseAssets_v1.0.2.md`](ReleaseAssets_v1.0.2.md) | v1.0.2 发行资源归档 |
| [`ReleaseAssets_v1.0.2.sha256`](ReleaseAssets_v1.0.2.sha256) | — |

## 历史记录（`archive/`）

按日期的一次性实现/排查记录与早期的发行说明，保留备查，不再更新。

| 文档 | 内容 |
|---|---|
| [`AutomaticRoutePlanning_20260908.md`](archive/AutomaticRoutePlanning_20260908.md) | 自动路径规划首版 |
| [`AutoReplanCore_20260908.md`](archive/AutoReplanCore_20260908.md) | 实时路线重排核心 |
| [`F8NearbyGuide_20260909.md`](archive/F8NearbyGuide_20260909.md) | F8 无活动路线时打开附近攻略 |
| [`GamepadAdaptationDesign_20260908.md`](archive/GamepadAdaptationDesign_20260908.md) | 标准 Xbox 手柄适配设计 |
| [`GamepadEntryDiagnosis_20260908.md`](archive/GamepadEntryDiagnosis_20260908.md) | LB 入口无响应排查（2026-09-08） |
| [`GamepadMapRecognition_20260908.md`](archive/GamepadMapRecognition_20260908.md) | 手柄大地图识别问题分析 |
| [`GamepadReturnRefusal_20260908.md`](archive/GamepadReturnRefusal_20260908.md) | LB → B 返回游戏失败修复 |
| [`IndependentMaps_20260909.md`](archive/IndependentMaps_20260909.md) | 非 World 地图接入 |
| [`KuroProgressSyncPlan_20260916.md`](archive/KuroProgressSyncPlan_20260916.md) | 库街区浏览器扩展与点位进度同步计划 |
| [`MapOverlayAndFilterFix_20260907.md`](archive/MapOverlayAndFilterFix_20260907.md) | 地图标记退出、过滤器及诊断布局修复 |
| [`MarkerFeatures_20260908.md`](archive/MarkerFeatures_20260908.md) | 重叠点位、攻略与本地完成记录 |
| [`NavigationRefresh_20260908.md`](archive/NavigationRefresh_20260908.md) | IMao 手柄、实时路线与界面更新 |
| [`OverlayCaptureNeverPublishes_20260918.md`](archive/OverlayCaptureNeverPublishes_20260918.md) | 2026-09-18 覆盖层一直停在「等待定位」：BitBlt 回退一帧都发布不出去 |
| [`OverlayInputLatencyFix_20260917.md`](archive/OverlayInputLatencyFix_20260917.md) | 2026-09-17 覆盖层输入延迟修复 |
| [`OverlayMotionOptimization_20260907.md`](archive/OverlayMotionOptimization_20260907.md) | 地图标记运动流畅度优化（2026-09-07） |
| [`Release-2026.9.17.1.md`](archive/Release-2026.9.17.1.md) | 2026.9.17.1 库街区点位进度同步 |
| [`Release-2026.9.17.2.md`](archive/Release-2026.9.17.2.md) | 2026.9.17.2 库街区点位进度同步 |
| [`Release-2026.9.17.3.md`](archive/Release-2026.9.17.3.md) | 2026.9.17.3 库街区点位进度同步 |
| [`Release-2026.9.17.4.md`](archive/Release-2026.9.17.4.md) | 2026.9.17.4 修复无法更新的问题 |
| [`Release-2026.9.17.5.md`](archive/Release-2026.9.17.5.md) | 2026.9.17.5 |
| [`Release-2026.9.17.6.md`](archive/Release-2026.9.17.6.md) | 2026.9.17.6 |
| [`Release-2026.9.18.2.md`](archive/Release-2026.9.18.2.md) | 2026.9.18.2 |
| [`Release-2026.9.9.5.md`](archive/Release-2026.9.9.5.md) | 2026.9.9.5 攻略快捷键修复 |
| [`Release-2026.9.9.6.md`](archive/Release-2026.9.9.6.md) | 2026.9.9.6 完整工作区更新 |
| [`Release-KuroMapSync-0.1.1.md`](archive/Release-KuroMapSync-0.1.1.md) | IMao 库街区同步扩展 0.1.1（手动安装版） |
| [`ResourceSelectionRegression_20260909.md`](archive/ResourceSelectionRegression_20260909.md) | F8 更新后地图定位失效：实际资源选择回归 |
| [`SparseMapViewport_20260909.md`](archive/SparseMapViewport_20260909.md) | 中央海面导致的大地图定位失败 |
| [`TethysVideoReview_20260909.md`](archive/TethysVideoReview_20260909.md) | Tethys 视频检查 |
