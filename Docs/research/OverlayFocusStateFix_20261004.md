# IMAO 切屏状态保留修复（2026-10-04）

本次按用户确认的范围实施：切屏隐藏覆盖层并暂停输入，回到原地图后恢复已有路线与手绘状态；真正关闭大地图仍取消活动手绘。分支 `codex/fix-overlay-focus-state`，基于 `e664c1d`，仅交付本地 `out/map-test`。

## 根因与实现

之前的 `未渲染地图 → DrawMarkerInteraction::Clear → MapUnavailable → handDraft.Cancel` 将失焦与关闭地图混为一谈。

- `MapUnavailable` 仅撤销地图上下文与旧输入代次，保留手绘点位、身份、顺序、活动状态、规划选择及预览。
- `MapClosed` 单独处理确认关闭；暂停后仍能取消活动手绘，重复通知幂等；已结束未保存草稿仍保留。
- 检测线程仅在截图新鲜、游戏可见且未最小化、显示上下文有效、稳定 Gameplay 与当前小地图证据同时成立时通知关闭。Unknown、失焦、截图过期和配准失败不会取消草稿。
- 渲染暂停仍清理命中区域、点击、鼠标捕获和未完成手势。恢复必须匹配档案、原地图、新鲜投影和输入代次；不重新开启手绘。浏览器中的 Escape/Ctrl+Z 不归手绘接管。
- 新增 `map-context-suspended`、`map-context-resumed`、`map-context-closed` 日志；IPC、存储格式、样式与快捷键保持现有契约。
- 独立审查发现检测线程取消草稿与渲染快照之间的竞争；已改为从最终局部快照同时计算绘制标志并读取草稿，避免空 optional 解引用。

## 自动化验证

证据目录：`out/overlay-focus-state-20261004`。最初针对旧服务的回归有 11 项行为断言失败（`red-service.log`）；新接口测试也在实施前编译失败（`red-interfaces-build.log`）。修复后 Release CoreHost 构建通过。

通过的独立日志：`service-normal.log`、`service-save-failure.log`、`service-map-suspension.log`、`IMaoHandDrawnRouteTests.log`、`IMaoRoutePlanningTests.log`、`IMaoMarkerTests.log`、`IMaoOptimizationTests.log`。新增手绘与暂停模式已接入 `scripts/Test-Runtime.ps1`。

暂停回归包含混合官方/自由点、重复暂停、恢复追加/撤销/保存、空/单点/结束草稿、真正关闭与重复关闭、旧代次点击、不同地图、档案切换、会话停止，以及规划选择与预览。输入资格与关闭证据使用独立单元断言。实际 Alt+Tab 与投影绘制效果仍需实机确认。

最初将新回归追加到完整服务套件末尾时，已有用例已替换点位目录，导致测试夹具身份不匹配；现用独立 `map-suspension` 进程运行，不改变正确的业务断言。

## 测试树交付

使用已有自包含 2026.10.3.1 托管 Release 产物（来源 `f68cb47`，托管相关源码与当前分支无差异）及本次新构建 CoreHost。不重新下载、不发布、不修改玩家安装。`build-info.json` 与 `native-build-info.json` 如实记录本次未提交源码及 SHA-256，并注明托管复用来源。

构建前已备份测试树快照、联接、区域包清单和关键二进制哈希。刷新使用 `Refresh-MapTestBinaries.ps1`，其时间戳判断后再逐文件核对 SHA-256；必要时补齐不一致的根文件与运行时子目录。保留 Assets、MapTiles 联接及 no-baseline 布局。部署时间和逐文件哈希见证据目录的 `deployment.json`、`deployed-file-hashes.json`；资源预检见 `resource-preflight.log`，刷新与分层检查见 `refresh-map-test.log`。

测试入口：`out/map-test/IMao-WinUI.exe`（管理员）。启动前完全退出旧 IMAO，确认无更新激活路径把启动重定向到另一版本。

## 实机验收

1. 在大地图开启手绘，添加官方点和自由点，记住编号；短时间 Alt+Tab 再回来，直接追加，确认编号与连接线延续。
2. 切到攻略停留较长时间，在浏览器按 Escape/Ctrl+Z，再回游戏；草稿仍在，游戏内撤销只撤销最后一个点。
3. 最小化、恢复、连续切屏，确认不会提交旧点击或中断拖拽；可结束绘制并保存。
4. 在活动手绘时真正关闭大地图，再打开，确认活动草稿已取消；结束但未保存的草稿保持既有行为。
5. 换档案、换地图或结束游戏会话，确认旧输入与投影不会进入新上下文。
