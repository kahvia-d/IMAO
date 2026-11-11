# IMao 文档索引

按用途查找文档；目录分类不表示方案已完成或实机验收通过。旧名 WWMAP-TOOLS 与 IMao/IMAO 指同一项目。

## 常用入口

| 要做什么 | 入口 |
|---|---|
| 构建与开发 | [可复现构建](guides/Build_zh-Hans.md) |
| 使用云鸣潮 | [窗口选择与手动验收](guides/CloudWuthering_zh-Hans.md) |
| 库街区进度同步 | [Chrome / Edge 安装说明](KuroMapSyncInstall.md) · [隐私说明](KuroMapSyncPrivacy.md) |
| 发布与更新 | [云端构建、本地签名](CloudRelease.md) · [程序更新](ProgramUpdates.md) · [资源更新](ResourceUpdates.md) |
| 继续坐标研究 | [YOLO 续接](guides/CoordinateYoloResume_zh-Hans.md) · [成果保留](guides/ResearchArtifactRetention_zh-Hans.md) |
| 查漏补缺名录 | [维护指南](guides/MarkerDetectabilityMaintenance.md) · [新视频流程](guides/MarkerDetectabilityWorkflow_zh-Hans.md) |
| 未完成事项 | [OpenWork](OpenWork.md)（按条目日期及验证记录核对） |
| 新增或维护文档 | [文档维护规则](guides/DocumentationMaintenance.md) |

## 当前状态来源

版本以 [Version.props](../Version.props) 为准；发布事务见 [release-state.json](../updates/release-state.json)，签名清单见 [stable.json](../updates/stable.json)。2026-11-12 整理时源码版本为 **2026.11.11.2**。

[CloudReleaseNotes.md](CloudReleaseNotes.md) 是当前发布正文；`releases/` 和 `archive/` 中的 Release 文件是历史留档，并非完整版本列表，不能用最后一篇文档推断当前版本。

## 分类目录

下面逐项登记全部文档。`research/` 保存当时的实验与故障证据；`plans/` 同时包含待办和实施记录；`archive/` 保存既有归档及明确退役资料。日期较早不等于结论失效。

## 固定路径文档

| 文档 | 内容 / 阅读提示 |
|---|---|
| [CloudRelease.md](CloudRelease.md) | 云端构建、本地签名发布 |
| [CloudReleaseNotes.md](CloudReleaseNotes.md) | 发布正文；禁止添加内部维护备注 |
| [CoordinateOfflineEvaluation_zh-Hans.md](CoordinateOfflineEvaluation_zh-Hans.md) | 坐标 OCR 独立离线评测（2026-10历史对照，非当前YOLO入口） |
| [CoordinateSampleCapture_zh-Hans.md](CoordinateSampleCapture_zh-Hans.md) | 坐标识别素材采集 |
| [GameFrameCostAnalysis_20260918.md](GameFrameCostAnalysis_20260918.md) | 工具对游戏帧率的影响：玩家现场现象与成因分析 |
| [GameFrameDropAnalysis_20260917.md](GameFrameDropAnalysis_20260917.md) | 2026-09-17 游戏掉帧：工具侧开销分析与改动 |
| [KuroMapSyncInstall.md](KuroMapSyncInstall.md) | 库街区进度同步扩展：安装说明 |
| [KuroMapSyncPrivacy.md](KuroMapSyncPrivacy.md) | IMao 库街区进度同步扩展 · 隐私说明 |
| [MapRegionRefactor-Handoff.md](MapRegionRefactor-Handoff.md) | 地图资源分区域重构 · 交接说明 |
| [OpenWork.md](OpenWork.md) | 历史问题、后续修复与待验收项，按条目日期阅读 |
| [ProgramUpdates.md](ProgramUpdates.md) | 程序自更新 |
| [ReleaseAssets_v1.0.2.md](ReleaseAssets_v1.0.2.md) | v1.0.2 发行资源归档 |
| [ReleaseAssets_v1.0.2.sha256](ReleaseAssets_v1.0.2.sha256) | 发行资源 SHA-256 清单 |
| [ResourceUpdates.md](ResourceUpdates.md) | 程序与地图资源更新 |

## 使用与维护指南

| 文档 | 内容 / 阅读提示 |
|---|---|
| [Build_zh-Hans.md](guides/Build_zh-Hans.md) | 可复现构建（Windows x64） |
| [CloudWuthering_zh-Hans.md](guides/CloudWuthering_zh-Hans.md) | 云鸣潮网页版使用与测试 |
| [CoordinateYoloResume_zh-Hans.md](guides/CoordinateYoloResume_zh-Hans.md) | 坐标 YOLO 研究续接与清理保留清单 |
| [DocumentationMaintenance.md](guides/DocumentationMaintenance.md) | 文档维护与状态口径 |
| [KuroMapDataSource.md](guides/KuroMapDataSource.md) | 库街区地图数据同步 |
| [KuroMapSyncStoreListing.md](guides/KuroMapSyncStoreListing.md) | Chrome / Edge 商店入口、桥接身份与 Edge 历史上架材料 |
| [KuroSceneCalibrationSamples.md](guides/KuroSceneCalibrationSamples.md) | 新地区四点校准样本 |
| [KuroTileFeaturePacks.md](guides/KuroTileFeaturePacks.md) | 库街区瓦片特征包 |
| [MarkerDetectabilityMaintenance.md](guides/MarkerDetectabilityMaintenance.md) | 查漏补缺模式与名录维护 |
| [MarkerDetectabilityWorkflow_zh-Hans.md](guides/MarkerDetectabilityWorkflow_zh-Hans.md) | 不可探测点位视频研究：新增视频与完成后清理 |
| [ResearchArtifactRetention_zh-Hans.md](guides/ResearchArtifactRetention_zh-Hans.md) | 研究成果保留与恢复入口 |
| [RouteGuideFallbackAndSkip_zh-Hans.md](guides/RouteGuideFallbackAndSkip_zh-Hans.md) | 路线攻略回退与攻略内长按跳过（实施与验收记录） |
| [DocumentationPathMap.md](guides/DocumentationPathMap.md) | 156 篇文档的旧路径到新入口对照，供历史注释和配方追溯 |

## 设计与运行规则

| 文档 | 内容 / 阅读提示 |
|---|---|
| [LayeredMapFeaturePackPlan.md](architecture/LayeredMapFeaturePackPlan.md) | 分层地图特征包落地方案（草案，2026-09-22） |
| [LayeredMapOpenness_20260927.md](architecture/LayeredMapOpenness_20260927.md) | 开放分层地图 / 封闭分层地图（设计决定，2026-09-27） |
| [MapRegionOnDemandHandoff.md](architecture/MapRegionOnDemandHandoff.md) | 区域按需下载 / 删除 · 设计说明（续篇） |
| [MirrorChyanIntegration.md](architecture/MirrorChyanIntegration.md) | Mirror酱现行接入与历史方案 |
| [ProgramShardIncrementalPlan.md](architecture/ProgramShardIncrementalPlan.md) | 程序本体分片增量更新方案（已评审） |
| [VisualLocalization_zh-Hans.md](architecture/VisualLocalization_zh-Hans.md) | 小地图视觉定位 |

## 计划与交接

| 文档 | 内容 / 阅读提示 |
|---|---|
| [2026-10-14-english-coordinate-model.md](plans/2026-10-14-english-coordinate-model.md) | English coordinate model offline evaluation |
| [2026-11-11-overlay-sizing.md](plans/2026-11-11-overlay-sizing.md) | 游戏内独立缩放 Implementation Plan |
| [2026-11-11-startup-latency.md](plans/2026-11-11-startup-latency.md) | 启动提速 Implementation Plan |
| [2026-11-11-window-placement.md](plans/2026-11-11-window-placement.md) | 主窗口位置恢复 Implementation Plan |
| [2026-11-12-docs-organization.md](plans/2026-11-12-docs-organization.md) | Docs 分类整理实施记录 |
| [AutoRoutePlanningTests_20260908.md](plans/AutoRoutePlanningTests_20260908.md) | 自动路线规划测试命令 |
| [CloudWutheringAdaptationPlan_20261103.md](plans/CloudWutheringAdaptationPlan_20261103.md) | 云鸣潮网页版适配实现计划与实现记录 |
| [CoordinateHandoffStability_20261016_zh-Hans.md](plans/CoordinateHandoffStability_20261016_zh-Hans.md) | 实机交接稳定与绘制配准恢复 |
| [KuroPsMaptest-Handoff.md](plans/KuroPsMaptest-Handoff.md) | 库街区点位与 PS 手柄测试交付 |
| [MinimapMarkerRenderPerfPlan_zh-Hans.md](plans/MinimapMarkerRenderPerfPlan_zh-Hans.md) | IMAO 小地图标记渲染掉帧优化任务书 |
| [MiniMapOverlayDesign_zh-Hans.md](plans/MiniMapOverlayDesign_zh-Hans.md) | 小地图局部覆盖层（Mini Overlay）正式设计 |
| [NativeRoutePlanningMap_20260928.md](plans/NativeRoutePlanningMap_20260928.md) | Native (C++) Route-Planning Core — Evidence Map |
| [NoExtrapolationPlan_20261101.md](plans/NoExtrapolationPlan_20261101.md) | 105 删除位置外推与统一标记投影计划 |
| [OcrAsyncFloorPlan_20261101.md](plans/OcrAsyncFloorPlan_20261101.md) | 107：异步楼层分类与 OCR GPU 设置实施计划 |
| [OcrDelayPlan_20261101.md](plans/OcrDelayPlan_20261101.md) | 106 OCR 结果消费延迟诊断计划 |
| [OldPcLocalizationPlan_20261031.md](plans/OldPcLocalizationPlan_20261031.md) | 旧电脑定位与录制隔离测试计划 |
| [PerformanceMemoryPlan_20261020.md](plans/PerformanceMemoryPlan_20261020.md) | 地图特征内存与全局搜索分段计划 |
| [PerformanceOptimizationPlan_zh-Hans.md](plans/PerformanceOptimizationPlan_zh-Hans.md) | 启动加载与坐标识别优化实施计划 |
| [PixelAnchorHandoffRecovery_20261018_zh-Hans.md](plans/PixelAnchorHandoffRecovery_20261018_zh-Hans.md) | 实测像素锚点恢复与独立来源健康 |
| [ReproCaptureReliabilityPlan_20261031.md](plans/ReproCaptureReliabilityPlan_20261031.md) | 短时完整采集可靠性修改计划 |
| [RouteCollectionsAndTransfer_20261001.md](plans/RouteCollectionsAndTransfer_20261001.md) | 方案与实施记录；偏差、测试及遗留见 §11 |
| [RuntimeMemoryPlan_20261020.md](plans/RuntimeMemoryPlan_20261020.md) | Runtime Memory Implementation Plan |
| [StatefulDisplayPlan_20261101.md](plans/StatefulDisplayPlan_20261101.md) | 104 状态式显示与位置保持 |
| [VisibilityAndDeltaPlan_20261031.md](plans/VisibilityAndDeltaPlan_20261031.md) | 显示许可与差量测试包实施计划 |
| [VisualIndexVocabularySharing.md](plans/VisualIndexVocabularySharing.md) | 视觉索引词汇表共享：方案与迁移风险 |

## 研究与故障记录

| 文档 | 内容 / 阅读提示 |
|---|---|
| [CandidatePackRedundancy.md](research/CandidatePackRedundancy.md) | 候选包是否已被梦州区域包取代 · 实测 |
| [ClientCaptureCropFix_20261024.md](research/ClientCaptureCropFix_20261024.md) | WGC 客户端裁剪修正（2026-10-24） |
| [CloudWutheringCaptureValidation_20261103.md](research/CloudWutheringCaptureValidation_20261103.md) | 云鸣潮捕获验证记录（2026-11-03） |
| [CoordinateAnnotationExpansion_20261015_zh-Hans.md](research/CoordinateAnnotationExpansion_20261015_zh-Hans.md) | 已有坐标截图的视觉标注扩充 |
| [CoordinateCaptureStudy_20261014_zh-Hans.md](research/CoordinateCaptureStudy_20261014_zh-Hans.md) | 同帧坐标与小地图采集：第一轮实机核验 |
| [CoordinateCoverageFinetune_20261015_zh-Hans.md](research/CoordinateCoverageFinetune_20261015_zh-Hans.md) | 草地、雪地覆盖与正 X 回放微调对照（2026-10-15） |
| [CoordinateExpandedEvaluation_20261015_zh-Hans.md](research/CoordinateExpandedEvaluation_20261015_zh-Hans.md) | 扩展标注集重评分与坐标训练裁片 |
| [CoordinateFinetune_20261015_zh-Hans.md](research/CoordinateFinetune_20261015_zh-Hans.md) | 坐标 OCR 首轮离线微调（2026-10-15） |
| [CoordinateFlightEffectRetrain_20261016_zh-Hans.md](research/CoordinateFlightEffectRetrain_20261016_zh-Hans.md) | 飞行特效素材补训与留出回放 |
| [CoordinateImageGap_20261015_zh-Hans.md](research/CoordinateImageGap_20261015_zh-Hans.md) | 坐标／日期图像分区对照 |
| [CoordinateIndependentEvaluation_20261016_zh-Hans.md](research/CoordinateIndependentEvaluation_20261016_zh-Hans.md) | 新会话素材整理与冻结模型独立评测 |
| [CoordinateInputAlignment_20261015_zh-Hans.md](research/CoordinateInputAlignment_20261015_zh-Hans.md) | 坐标 OCR 实际输入、导出与日期边界诊断 |
| [CoordinateLineExperiment_20261015_zh-Hans.md](research/CoordinateLineExperiment_20261015_zh-Hans.md) | 坐标文字行、等比缩放与日期边界：离线实验 |
| [CoordinateOwnYoloExpansion_20261015_zh-Hans.md](research/CoordinateOwnYoloExpansion_20261015_zh-Hans.md) | IMao 自训字符检测：扩充不同坐标与背景 |
| [CoordinateOwnYoloPilot_20261015_zh-Hans.md](research/CoordinateOwnYoloPilot_20261015_zh-Hans.md) | IMao 自训字符检测：首轮拟合与 ONNX 链路 |
| [CoordinateOwnYoloXY_20261015_zh-Hans.md](research/CoordinateOwnYoloXY_20261015_zh-Hans.md) | IMao 自训 YOLO：XY 必需，Z 可选 |
| [CoordinatePairedFinetune_20261015_zh-Hans.md](research/CoordinatePairedFinetune_20261015_zh-Hans.md) | 静止草地配对补充训练与上下文对照（2026-10-15） |
| [CoordinateRegressionRecovery_20261016_zh-Hans.md](research/CoordinateRegressionRecovery_20261016_zh-Hans.md) | 飞行补训后的退步恢复与单张推理核验 |
| [CoordinateSignLayout_20261015_zh-Hans.md](research/CoordinateSignLayout_20261015_zh-Hans.md) | IMao 坐标符号：固定 X 起点与 Y 相对间距校验 |
| [CoordinateSignRecovery_20261016_zh-Hans.md](research/CoordinateSignRecovery_20261016_zh-Hans.md) | 自训坐标模型：图像确认补负号与训练补充 |
| [CoordinateStationaryGrass_20261015_zh-Hans.md](research/CoordinateStationaryGrass_20261015_zh-Hans.md) | 静止位置的清晰／草叶遮挡成组素材（2026-10-15） |
| [CoordinateTargetedRetrain_20261016_zh-Hans.md](research/CoordinateTargetedRetrain_20261016_zh-Hans.md) | 浅色数字与五位数坐标的针对性补训 |
| [CoordinateYoloCropRobustness_zh-Hans.md](research/CoordinateYoloCropRobustness_zh-Hans.md) | 坐标 YOLO 裁剪鲁棒性 |
| [CoordinateYoloIntegration_20261016_zh-Hans.md](research/CoordinateYoloIntegration_20261016_zh-Hans.md) | 自训 YOLO 与图像定位交接初版 |
| [CpuMemoryInvestigation_20261020.md](research/CpuMemoryInvestigation_20261020.md) | 游戏内定位与大地图匹配 CPU / 内存占用分析 |
| [DetectorMigrationDecision_20261008.md](research/DetectorMigrationDecision_20261008.md) | 换检测器（SURF → SIFT）：决策实验与结论（2026-10-08） |
| [DiagnosticReproCapture_20261023.md](research/DiagnosticReproCapture_20261023.md) | 短时完整诊断复现采集 |
| [FarmMode_20260927.md](research/FarmMode_20260927.md) | 刷怪采集模式（自动标记与每日重置）设计 |
| [FeatureSizeComparison-20261002.md](research/FeatureSizeComparison-20261002.md) | Feature size and detector comparison (2026-10-02) |
| [GamepadCursor_20260908.md](research/GamepadCursor_20260908.md) | 大地图手柄光标点位修复 |
| [HandDrawnFreePoints_20261004.md](research/HandDrawnFreePoints_20261004.md) | 手绘自由点：本地进度、类型和地图显示 |
| [HandRouteEditing_20261005.md](research/HandRouteEditing_20261005.md) | 手绘路线编辑实施记录 |
| [ImageAnchoredOverlay_20260908.md](research/ImageAnchoredOverlay_20260908.md) | 地形图像跟随与小地图浮动修复（2026-09-08） |
| [KuroPointChanges_20261006.md](research/KuroPointChanges_20261006.md) | maptest 点位变化清单（2026-10-06） |
| [KuroSyncCredentialNaming_20260927.md](research/KuroSyncCredentialNaming_20260927.md) | 缺陷分析：已连接库街区，仍提示「此同步档案尚未连接库街区」 |
| [LayeredFloorIndexFrame_20260926.md](research/LayeredFloorIndexFrame_20260926.md) | 隐海试验场的分层地图识别不出来的根因：分层楼层索引的坐标系建错了 |
| [LayeredFloorRetention_20261001.md](research/LayeredFloorRetention_20261001.md) | 分层楼层该解除时不解除：地表就在洞穴正上方（2026-10-01，roysurface 天槎空间站） |
| [LayeredMapFalsePositive_Hukou_20260926.md](research/LayeredMapFalsePositive_Hukou_20260926.md) | 虎口山脉标记消失：分层地图误判复盘（2026-09-26） |
| [LayeredMapFeatureMeasurement_20260922.md](research/LayeredMapFeatureMeasurement_20260922.md) | 分层地图瓦片特征密度实测（2026-09-22） |
| [LegacyDataRecovery_20260927.md](research/LegacyDataRecovery_20260927.md) | 旧版本地数据修复（帮助页） |
| [LocalAccounts_20260926.md](research/LocalAccounts_20260926.md) | 本地多账本与库街区账号绑定（设计草案） |
| [MapMarkerFlicker_20260928.md](research/MapMarkerFlicker_20260928.md) | 大地图/小地图标记"一闪一闪"（玩家日志分析，2026-09-28） |
| [MapMarkerFlicker_20260928_evening_playerlog.md](research/MapMarkerFlicker_20260928_evening_playerlog.md) | 大地图标记闪烁：玩家 2026-09-28 傍晚实测的日志分析 |
| [MapMarkerFlicker_20261013_even.md](research/MapMarkerFlicker_20261013_even.md) | even 的大地图标记闪烁：大世界 / 下层金库对照（2026-10-13） |
| [MapMarkerFlicker_20261014_even_diagnostic.md](research/MapMarkerFlicker_20261014_even_diagnostic.md) | even 诊断包新日志：点位过滤阻塞与源帧过期（2026-10-14） |
| [MapMarkerFlickerAudit_20261031.md](research/MapMarkerFlickerAudit_20261031.md) | 大小地图闪烁复查（2026-10-31） |
| [MapRecognitionAudit_20260906.md](research/MapRecognitionAudit_20260906.md) | 2026-09-06 地图识别根因审计 |
| [MapTools_20260908.md](research/MapTools_20260908.md) | 地图工具台与附近点位操作 |
| [MarkerDetectabilityCollection_20261029.md](research/MarkerDetectabilityCollection_20261029.md) | 探测仪遗漏点位：合集离线整理（2026-10-29） |
| [MarkerDetectabilitySpike_20261029.md](research/MarkerDetectabilitySpike_20261029.md) | 黑海岸不可探测点位小样本核验 |
| [MarkerDetectabilityV2_20261029.md](research/MarkerDetectabilityV2_20261029.md) | 可探测性补充 v2：视频时间轴优先 |
| [MarkerGuideLayout_20260908.md](research/MarkerGuideLayout_20260908.md) | 攻略白边、默认位置与图片翻页 |
| [MarkerGuideWindow_20260908.md](research/MarkerGuideWindow_20260908.md) | 攻略浮窗开关与完成交互 |
| [MengshuTianluo_20260930.md](research/MengshuTianluo_20260930.md) | 梦枢天罗接入（2026-09-30，分支 `feature/mengshutianluo`） |
| [MinimapMarkerFlicker_20261018_even.md](research/MinimapMarkerFlicker_20261018_even.md) | even 小地图闪烁：同类读取瓶颈仍在（2026-10-18） |
| [MinimapMarkerFlickerFix_20261019_even.md](research/MinimapMarkerFlickerFix_20261019_even.md) | even 小地图标记批量读取修复（2026-10-19） |
| [MinimapReadoutProjection_20261018_zh-Hans.md](research/MinimapReadoutProjection_20261018_zh-Hans.md) | 固定朝向小地图的读数投影 |
| [MinimapReadoutPublication_20261018_zh-Hans.md](research/MinimapReadoutPublication_20261018_zh-Hans.md) | OCR绘制快照发布与SURF搜索状态分离 |
| [MinimapRecoveryFix_20260907.md](research/MinimapRecoveryFix_20260907.md) | 小地图跨区域恢复修复（2026-09-07） |
| [MinimapRetrievalAudit_20260907.md](research/MinimapRetrievalAudit_20260907.md) | 小地图跨区域定位排查（2026-09-07 晚间） |
| [OcrSignWitness_20260930.md](research/OcrSignWitness_20260930.md) | 坐标读数丢负号：为什么位移预算拦不住，以及现在由谁作证 |
| [OverlayContinuityFix_20260908.md](research/OverlayContinuityFix_20260908.md) | 小地图闪烁与大地图快拖错位（2026-09-08） |
| [OverlayDragRecovery_20261004.md](research/OverlayDragRecovery_20261004.md) | 拖动大地图误关闭手绘草稿（2026-10-04） |
| [OverlayFocusStateFix_20261004.md](research/OverlayFocusStateFix_20261004.md) | IMAO 切屏状态保留修复（2026-10-04） |
| [PlayerCursorStutter_20261020.md](research/PlayerCursorStutter_20261020.md) | 玩家大地图标记出现后鼠标卡顿：日志分析 |
| [ProgramUpdateDiskReuse_20261010.md](research/ProgramUpdateDiskReuse_20261010.md) | 硬链接等已实施；一次性迁移未实施，实际节省量仍按原文待验收 |
| [ProjectAudit_20260907.md](research/ProjectAudit_20260907.md) | 全项目审计与基础修复（2026-09-07） |
| [ReadoutContinuity_20261018_zh-Hans.md](research/ReadoutContinuity_20261018_zh-Hans.md) | 读数短暂断读恢复、接管节奏与提前发布 |
| [ReadoutDirectTest_20261018_zh-Hans.md](research/ReadoutDirectTest_20261018_zh-Hans.md) | OCR显示完全关闭预测与平滑的对照（2026-10-18） |
| [ReadoutJitterFix_20261018_zh-Hans.md](research/ReadoutJitterFix_20261018_zh-Hans.md) | OCR显示读数交接回跳修复（2026-10-18） |
| [ReadoutMotion_20261018_zh-Hans.md](research/ReadoutMotion_20261018_zh-Hans.md) | OCR接管调度、显示运动与完整缺口诊断 |
| [ReadoutScheduling_20261018_zh-Hans.md](research/ReadoutScheduling_20261018_zh-Hans.md) | OCR 接管调度稳定性（2026-10-18） |
| [Refactor_20260907.md](research/Refactor_20260907.md) | 核心重构记录（2026-09-07） |
| [ResourcePackagePickerFix-20260909.md](research/ResourcePackagePickerFix-20260909.md) | 离线导入入口修复记录（2026-09-09） |
| [ResourceUpdateValidation.md](research/ResourceUpdateValidation.md) | 资源更新系统验证记录（2026-09-09） |
| [RouteLibraryAndHandDrawn_20260928.md](research/RouteLibraryAndHandDrawn_20260928.md) | 路线库（保存/加载/切换/筛选）与手绘路线重做 —— 实施计划 |
| [RouteUiTrimAudit_20261001.md](research/RouteUiTrimAudit_20261001.md) | 路线功能界面精简 · 审阅（哪些只能在游戏内用、哪些放 UI 上没有意义） |
| [RouteUsability_20260908.md](research/RouteUsability_20260908.md) | 路线退出、删除与快捷键指南 |
| [RoyMaps_20260909.md](research/RoyMaps_20260909.md) | 罗伊冰原地图补充 |
| [SparseMinimapFusion_20260909.md](research/SparseMinimapFusion_20260909.md) | 稀疏小地图与辅助定位整合（2026-09-09） |
| [TestResourceStateIncident_20261020.md](research/TestResourceStateIncident_20261020.md) | 测试树切换地区导致玩家版地区列表缩减 |
| [TethysCalibration_20260909.md](research/TethysCalibration_20260909.md) | 泰缇斯四点校准与资源隔离 |
| [ViewportColdStartRanking_20261008.md](research/ViewportColdStartRanking_20261008.md) | 大地图冷启动：先用视觉索引排序，再决定搜什么 |
| [VisualIndexRebuildRegression_20261008.md](research/VisualIndexRebuildRegression_20261008.md) | 视觉索引重建：为什么重建后反而变差（2026-10-08） |
| [ZscjLayerAttribute_20260927.md](research/ZscjLayerAttribute_20260927.md) | 终声残卷在星炬学院不显示：核实报告（2026-09-27，分支 `investigate/zscj-layer-attribute`） |
| [DocumentationContentAudit_20261112.md](research/DocumentationContentAudit_20261112.md) | 正文时效复核：35份文档的修订证据与保留验收边界 |

## 发行说明

| 文档 | 内容 / 阅读提示 |
|---|---|
| [Release-2026.10.1.1.md](releases/Release-2026.10.1.1.md) | 2026.10.1.1 |
| [Release-2026.10.1.2.md](releases/Release-2026.10.1.2.md) | 2026.10.1.2 |
| [Release-2026.10.2.1.md](releases/Release-2026.10.2.1.md) | 2026.10.2.1 |
| [Release-2026.10.2.2.md](releases/Release-2026.10.2.2.md) | 2026.10.2.2 |
| [Release-2026.10.3.1.md](releases/Release-2026.10.3.1.md) | 2026.10.3.1 |
| [Release-2026.10.4.1.md](releases/Release-2026.10.4.1.md) | 2026.10.4.1 |
| [Release-2026.10.4.2.md](releases/Release-2026.10.4.2.md) | 2026.10.4.2 |
| [Release-2026.10.6.1.md](releases/Release-2026.10.6.1.md) | 2026.10.6.1 |
| [Release-2026.9.18.1.md](releases/Release-2026.9.18.1.md) | 2026.9.18.1 |
| [Release-2026.9.19.1.md](releases/Release-2026.9.19.1.md) | 2026.9.19.1 |
| [Release-2026.9.19.2.md](releases/Release-2026.9.19.2.md) | 2026.9.19.2 |
| [Release-2026.9.21.1.md](releases/Release-2026.9.21.1.md) | 2026.9.21.1 |
| [Release-2026.9.22.1.md](releases/Release-2026.9.22.1.md) | 2026.9.22.1 |
| [Release-2026.9.23.1.md](releases/Release-2026.9.23.1.md) | 2026.9.23.1 |
| [Release-2026.9.25.1.md](releases/Release-2026.9.25.1.md) | 2026.9.25.1 |
| [Release-2026.9.25.2.md](releases/Release-2026.9.25.2.md) | 2026.9.25.2 |
| [Release-2026.9.26.1.md](releases/Release-2026.9.26.1.md) | 2026.9.26.1 |
| [Release-2026.9.26.2.md](releases/Release-2026.9.26.2.md) | 2026.9.26.2 |
| [Release-2026.9.26.3.md](releases/Release-2026.9.26.3.md) | 2026.9.26.3 |
| [Release-2026.9.27.1.md](releases/Release-2026.9.27.1.md) | 2026.9.27.1 |
| [Release-2026.9.27.2.md](releases/Release-2026.9.27.2.md) | 2026.9.27.2 |
| [Release-2026.9.28.1.md](releases/Release-2026.9.28.1.md) | 2026.9.28.1 |
| [Release-2026.9.28.2.md](releases/Release-2026.9.28.2.md) | 2026.9.28.2 |
| [Release-2026.9.28.3.md](releases/Release-2026.9.28.3.md) | 2026.9.28.3 |
| [Release-2026.9.28.4.md](releases/Release-2026.9.28.4.md) | 2026.9.28.4 |
| [Release-2026.9.30.1.md](releases/Release-2026.9.30.1.md) | 2026.9.30.1 |
| [Release-2026.9.9.3.md](releases/Release-2026.9.9.3.md) | WWMAP-TOOLS 2026.9.9.3 发行候选 |

## 历史归档

| 文档 | 内容 / 阅读提示 |
|---|---|
| [AutomaticRoutePlanning_20260908.md](archive/AutomaticRoutePlanning_20260908.md) | 自动路径规划首版 |
| [AutoReplanCore_20260908.md](archive/AutoReplanCore_20260908.md) | 实时路线重排核心 |
| [Compile_en.md](archive/Compile_en.md) | 上游历史构建说明；当前入口见 Build_zh-Hans.md |
| [Compile_zh-Hans.md](archive/Compile_zh-Hans.md) | 上游历史构建说明；当前入口见 Build_zh-Hans.md |
| [DreamzhouCandidateFeaturePack.md](archive/DreamzhouCandidateFeaturePack.md) | Dreamzhou curated-location candidates（已退役） |
| [F8NearbyGuide_20260909.md](archive/F8NearbyGuide_20260909.md) | F8 无活动路线时打开附近攻略 |
| [GamepadAdaptationDesign_20260908.md](archive/GamepadAdaptationDesign_20260908.md) | 标准 Xbox 手柄适配设计 |
| [GamepadEntryDiagnosis_20260908.md](archive/GamepadEntryDiagnosis_20260908.md) | LB 入口无响应排查（2026-09-08） |
| [GamepadMapRecognition_20260908.md](archive/GamepadMapRecognition_20260908.md) | 手柄大地图识别问题分析 |
| [GamepadReturnRefusal_20260908.md](archive/GamepadReturnRefusal_20260908.md) | LB → B 返回游戏失败修复 |
| [IndependentMaps_20260909.md](archive/IndependentMaps_20260909.md) | 非 World 地图接入 |
| [KuroProgressSyncPlan_20260916.md](archive/KuroProgressSyncPlan_20260916.md) | 库街区浏览器扩展与点位进度同步计划 |
| [MapOverlayAndFilterFix_20260907.md](archive/MapOverlayAndFilterFix_20260907.md) | 地图标记退出、过滤器及诊断布局修复 |
| [MarkerFeatures_20260908.md](archive/MarkerFeatures_20260908.md) | 重叠点位、攻略与本地完成记录 |
| [NavigationRefresh_20260908.md](archive/NavigationRefresh_20260908.md) | IMao 手柄、实时路线与界面更新 |
| [OverlayCaptureNeverPublishes_20260918.md](archive/OverlayCaptureNeverPublishes_20260918.md) | 2026-09-18 覆盖层一直停在「等待定位」：BitBlt 回退一帧都发布不出去 |
| [OverlayInputLatencyFix_20260917.md](archive/OverlayInputLatencyFix_20260917.md) | 2026-09-17 覆盖层输入延迟修复 |
| [OverlayMotionOptimization_20260907.md](archive/OverlayMotionOptimization_20260907.md) | 地图标记运动流畅度优化（2026-09-07） |
| [Release-2026.9.17.1.md](archive/Release-2026.9.17.1.md) | 2026.9.17.1 库街区点位进度同步 |
| [Release-2026.9.17.2.md](archive/Release-2026.9.17.2.md) | 2026.9.17.2 库街区点位进度同步 |
| [Release-2026.9.17.3.md](archive/Release-2026.9.17.3.md) | 2026.9.17.3 库街区点位进度同步 |
| [Release-2026.9.17.4.md](archive/Release-2026.9.17.4.md) | 2026.9.17.4 修复无法更新的问题 |
| [Release-2026.9.17.5.md](archive/Release-2026.9.17.5.md) | 2026.9.17.5 |
| [Release-2026.9.17.6.md](archive/Release-2026.9.17.6.md) | 2026.9.17.6 |
| [Release-2026.9.18.2.md](archive/Release-2026.9.18.2.md) | 2026.9.18.2 |
| [Release-2026.9.9.5.md](archive/Release-2026.9.9.5.md) | 2026.9.9.5 攻略快捷键修复 |
| [Release-2026.9.9.6.md](archive/Release-2026.9.9.6.md) | 2026.9.9.6 完整工作区更新 |
| [Release-KuroMapSync-0.1.1.md](archive/Release-KuroMapSync-0.1.1.md) | IMao 库街区同步扩展 0.1.1（手动安装版） |
| [ResourceSelectionRegression_20260909.md](archive/ResourceSelectionRegression_20260909.md) | F8 更新后地图定位失效：实际资源选择回归 |
| [SparseMapViewport_20260909.md](archive/SparseMapViewport_20260909.md) | 中央海面导致的大地图定位失败 |
| [TethysVideoReview_20260909.md](archive/TethysVideoReview_20260909.md) | Tethys 视频检查 |

## 配套资料

图片保留于 `images/`。本机长期记忆为根目录 `MEMORY.md`（不进 Git）；小型发布证据在 `out/evidence/`。固定路径与迁移维护规则见[文档维护](guides/DocumentationMaintenance.md)。
