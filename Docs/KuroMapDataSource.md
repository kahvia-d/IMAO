# 库街区地图数据同步

`scripts/Sync-KuroMapData.ps1 -Check` 从库街区公开 HTTPS 地图接口下载到临时目录、校验并报告差异，不改动仓库。`-Apply` 在全部 state、JSON 和 PNG 图标通过校验后，更新 `Assets/KuroMap` 与八个场景的点位数据。

当前公开数据按九个顶层 state 发布：8（World）、900（Tethys）、905（Fabricatorium）、903（Avinoleum）、906（Lahai）、902（LowerVault）、909（Darkplain）、910（TimeRiftRuins）、912（MengshuTianluo，梦枢天罗）。后四个 state 的点位会写到外部运行时 JSON，以避免修改旧 DLL 资源；`floorId`、`level` 等原始字段会原样保留。

`scripts/Sync-KuroMapData.ps1` 里这四个 state 的 `supported` 是 **`$false`**：它们的运行时点位由
`scripts/Publish-KuroMapNewStates.ps1` 单独发布。这个区分是承重的——**只有进了 `filter-items.json` 的 id 才会
无条件出现在筛选页**（`StringItems.cs` 对 `new-state-filter-items.json` 才按场景开放状态过滤），
所以把一个还没开放的新地区标成 supported 会让它的物品在场景获批之前就出现在筛选里。

首次接入新区用 `scripts/Publish-KuroMapNewStates.ps1`。它只从已归档的 state 生成四份新的运行时点位文件、一个附加筛选表和一个“物品 ID 属于哪个场景”的表，不会覆盖全量快照、图标或既有 `filter-items.json`。脚本严格核对当前归档快照的数量：下层金库 37 类/238 点、黯原 44 类/702 点、时隙废都 9 类/59 点、梦枢天罗 20 类/231 点；上游数量变化时会拒绝发布，必须先人工审查新快照。

新场景是否对用户开放由 `Assets/KuroMap/scene-validation.json` 单独控制，默认均为 `false`。这意味着点位资料可先保存并参与制作，但在四点校准、独立瓦片特征包和实机验证全部通过前，不会加入筛选、物品标注或小地图定位。校准和开放步骤见 [KuroSceneCalibrationSamples.md](KuroSceneCalibrationSamples.md)。

同步产物保留原始 `position.json`、`catalog.json`、国家层级、资源版本、时间、哈希、条目统计和未接入原因。图标从库街区静态公开资源下载，使用 ASCII 文件名并由 `icon-manifest.json` 映射；运行时优先读取它们，缺失时回退到 DLL 内嵌 PNG。**图标文件名按物品 id 稳定分配**（老 id 保留原名，只有新 id 取一个没人占用的编号）：早先按"排序后的位次"编号，一个新 state 就会把几百张图标整体改名重写。id 消失时它的文件会在下一次 `-Apply` 时删掉，因为清单是这些名字的唯一引用。筛选页会加载 `filter-items.json` 与经开放检查的附加筛选表；既有内置翻译优先。

数据来自库街区大地图公开前端数据。同步是人工触发的快照更新；在分发或长期托管前，请持续遵守上游网站和游戏的适用条款。
