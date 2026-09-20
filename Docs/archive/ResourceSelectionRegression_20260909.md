# F8 更新后地图定位失效：实际资源选择回归

## 原因

21:23 运行日志加载了 Tethys：7291 个特征，scene=2。21:39 更新界面程序后，资源会话启用了用户目录中之前安装的 `resources-2026.9.9.3`，只包含两个 World 扩展包和一个候选包，不包含 Tethys。旧基础库中的重复地形因此被当成 World，点位场景和校准也随之错误。

实际启用配置位于 `%LOCALAPPDATA%/IMao-WinUI/ResourceUpdates/activation.json`，旧运行快照为 `snapshots/runtime-f62263aff7a3734c1124019a.json`。此前只检查了发行目录的当前资源，未检查 ResourceSnapshotService 的实际选择。这是部署验证遗漏；F8 功能代码本身没有改变定位算法。

## 修复

通过 `scripts/Restore-BundledResources.ps1` 对当前内置资源执行 CoreHost 完整预检，备份激活配置，再在资源更新锁内原子切换到 `bundled-2026.9.9.4`。保留旧资源包及上一版本选择，未修改本地点位完成记录。已安装的 F8 界面文件哈希保持一致。

使用已安装的 `IMao-WinUI.Core.dll` 中 ResourceSnapshotService 初始化真实用户资源目录，确认实际启动快照包含 Tethys 和当前校准，并再次通过 CoreHost 资源预检。没有自动改变软件对外部资源包的通用优先级，以免破坏用户正常的资源更新和回退选择。

视觉回归工具新增可选 manifest 字段 `resourceSnapshot`（绝对路径）：经 ResourceSnapshotValidation 验证后直接使用生产 RuntimeFeatureRepository 加载完整快照，禁止同时混入 diagnosticPacks。以前不传此字段的目录回放入口继续保留。

## 验证

全部证据位于 `out/resource-selection-20260909/`。

- 旧实际快照：4 张泰缇斯大地图全部错误，场景误判 World。
- 恢复后实际快照：相同 4 张截图全部通过，scene=2。
- 中央海面截图：两次回放均通过，181/184 几何内点。
- 30 帧小地图视频：26 帧正确原始强匹配，13 帧通过连续确认发布，错误发布 0，与此前结果一致。
- 实际启动快照的 resourcesReady、viewportReady、visualReady 全为 true。

本轮恢复的是更新前定位能力。此前孤立礁石小地图的稀疏地形困难仍保留为未完成问题。没有以离线回放替代真实游戏重启后的验证。
