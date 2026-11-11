# 拖动大地图误关闭手绘草稿（2026-10-04）

用户初轮反馈切屏状态保留看起来正常，但拖动地图偶尔识别失败后，路线消失且无法恢复。本次在 `codex/fix-overlay-focus-state` 上继续修正已批准范围内的误取消路径。

## 本机证据

`%LOCALAPPDATA%/IMao-WinUI/Logs/events-20261004.jsonl` 中，14:33:59 的帧 4003/4006：

- 罗盘模板验证仍成功，agreement = 0.998332；缩放控件暂时不可见。
- 任务图标 SURF 误命中 6 个，`rawMinimap=1`。
- 状态从 BigMap 变成 Gameplay，随后 `map-context-closed points=20`；同一秒又回到 BigMap。

14:34:57 再出现同类关闭通知，取消 11 点草稿。因此这次并非 `MapUnavailable` 删除了数据，而是弱任务图标命中绕过了仍然有效的大地图罗盘证据，误触发 `MapClosed`。截取证据留在 `out/overlay-drag-recovery-20261004/player-evidence.jsonl`。

## 修正

`MapFrameEvidence::GameplayHudVisible` 使用当前帧完整大地图控件或模板验证的罗盘排除弱小地图命中；状态机与可见性共享这条规则。当前明确的大地图证据无需等待小地图缺失计时。`App::Thread_DetectGameState` 同样归一化 HUD，并阻止矛盾图像被学习为小地图模板。

旧 viewport anchor 与罗盘颜色不压过真正的当前游戏 HUD。真正回到 Gameplay 仍需稳定状态、当前 HUD、有效游戏显示上下文和新鲜截图，才执行活动草稿取消。识别完全丢失时仍暂停，重新配准后由已有草稿恢复。

新增 `map-hud-conflict decision=keep-big-map` 日志，仅在进入冲突时记录一次。没有改变手绘快捷键、保存格式或 IPC。

## 验证与交付

按玩家日志重建冲突序列，先在旧规则上出现 8 项失败（`red-optimization.log`），修正后完整优化套件通过（`green-optimization.log`）。覆盖多次冲突、识别缺失、识别恢复、控件冲突和真正关闭；已有独立手绘暂停恢复回归也通过（`service-map-suspension.log`）。一次只读增量审查未发现实质问题。

继续使用本地 2026.10.3.1 自包含托管 Release 产物及本次新核心，刷新 `out/map-test`，不发布、不下载、不修改玩家目录。构建前备份资源快照、联接及全部 11 个可达分层索引；刷新后逐文件 SHA-256、索引、快照、区域包、启动激活状态和原生资源预检证据见同目录 `deployment.json`、`deployed-file-hashes.json`、`refresh-map-test.log`、`resource-preflight.log`。部署时刻以 `deployment.json.deployedBeijing` 为准。

待用户实机确认：手绘加入若干官方点和自由点后连续拖动、缩放，经历一次标记消失与恢复，确认编号、连接线和活动状态仍在，可以直接继续画；同时验证真正关闭地图仍取消活动草稿。
