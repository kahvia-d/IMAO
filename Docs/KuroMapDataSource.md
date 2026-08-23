# 库街区地图数据同步

`scripts/Sync-KuroMapData.ps1 -Check` 从库街区公开 HTTPS 地图接口下载到临时目录、校验并报告差异，不改动仓库。`-Apply` 在全部 state、JSON 和 PNG 图标通过校验后，更新 `Assets/KuroMap` 与五个已经具备运行时坐标支持的点位文件。

当前公开数据按八个顶层 state 发布：8（World）、900（Tethys）、905（Fabricatorium）、903（Avinoleum）、906（Lahai）会进入运行时；902、909、910 只会完整归档并在同步报告中标记为“待接入”。它们尚没有本项目所需的坐标换算、地图底图与 SURF 特征库，不能仅凭点位 JSON 宣称可用。

同步产物保留原始 `position.json`、`catalog.json`、国家层级、资源版本、时间、哈希、条目统计和未接入原因。图标从库街区静态公开资源下载，使用 ASCII 文件名并由 `icon-manifest.json` 映射；运行时优先读取它们，缺失时回退到 DLL 内嵌 PNG。筛选页会加载 `filter-items.json` 中的新官方中文名称，既有内置翻译优先。

数据来自库街区大地图公开前端数据。同步是人工触发的快照更新；在分发或长期托管前，请持续遵守上游网站和游戏的适用条款。
