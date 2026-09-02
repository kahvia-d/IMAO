# 库街区地图数据同步

`scripts/Sync-KuroMapData.ps1 -Check` 从库街区公开 HTTPS 地图接口下载到临时目录、校验并报告差异，不改动仓库。`-Apply` 在全部 state、JSON 和 PNG 图标通过校验后，更新 `Assets/KuroMap` 与八个场景的点位数据。

当前公开数据按八个顶层 state 发布：8（World）、900（Tethys）、905（Fabricatorium）、903（Avinoleum）、906（Lahai）、902（LowerVault）、909（Darkplain）、910（TimeRiftRuins）。后三个 state 的点位会写到外部运行时 JSON，以避免修改旧 DLL 资源；`floorId`、`level` 等原始字段会原样保留。

首次接入三块新区可用 `scripts/Publish-KuroMapNewStates.ps1`。它只从已归档的三个 state 生成三份新的运行时点位文件、一个附加筛选表和一个“物品 ID 属于哪个场景”的表，不会覆盖全量快照、图标或既有 `filter-items.json`。脚本严格核对当前归档快照的数量：下层金库 37 类/238 点、黯原 43 类/673 点、时隙废都 9 类/59 点；上游数量变化时会拒绝发布，必须先人工审查新快照。

新场景是否对用户开放由 `Assets/KuroMap/scene-validation.json` 单独控制，默认均为 `false`。这意味着点位资料可先保存并参与制作，但在四点校准、独立瓦片特征包和实机验证全部通过前，不会加入筛选、物品标注或小地图定位。校准和开放步骤见 [KuroSceneCalibrationSamples.md](KuroSceneCalibrationSamples.md)。

同步产物保留原始 `position.json`、`catalog.json`、国家层级、资源版本、时间、哈希、条目统计和未接入原因。图标从库街区静态公开资源下载，使用 ASCII 文件名并由 `icon-manifest.json` 映射；运行时优先读取它们，缺失时回退到 DLL 内嵌 PNG。筛选页会加载 `filter-items.json` 与经开放检查的附加筛选表；既有内置翻译优先。

数据来自库街区大地图公开前端数据。同步是人工触发的快照更新；在分发或长期托管前，请持续遵守上游网站和游戏的适用条款。
