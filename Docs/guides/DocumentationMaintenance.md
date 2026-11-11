# 文档维护与状态口径

2026-11-12 按用户确认的“分类移动＋重写索引”整理。

## 放在哪里

| 目录 | 用途 | 状态口径 |
|---|---|---|
| `guides/` | 使用、构建、采集和维护入口 | 随核对更新；写清验证日期 |
| `architecture/` | 系统设计、规则和协议 | 保留需求来源及限制，不能把实现当需求 |
| `research/` | 实验、故障、性能与实现证据 | 历史结果保留，失败实验不改成通过 |
| `plans/` | 计划与交接、实施记录 | 不因文件名含 Plan 就判未实施；看实施与遗留章节 |
| `releases/` | 各版本发行说明 | 不覆盖旧发行事实；目录不保证每个发布版本都有单独文件 |
| `archive/` | 既有归档、明确退役或上游历史说明 | 不作为当前操作入口；保留原始内容 |

## 维护规则

- 新文档必须在 [Docs/README.md](../README.md) 的对应分类登记一行；日期用于事件记录，长期指南不靠文件名日期判断新旧。
- 全部操作命令默认在仓库根目录执行。Markdown 链接按文档所在目录计算；代码块中的 `scripts/`、`Assets/`、`out/` 等仍是仓库根路径。
- 文档状态与代码冲突时先依据用户授权核对历史提交和现有实现；没有授权就询问。实现存在不等于实机验收通过，提交记录不等于线上已发布。
- 版本看根目录 `Version.props`；发布事务看 `updates/release-state.json`；签名清单看 `updates/stable.json`。发行说明的最后留存日期不能替代当前版本。
- 修订历史文档时更新导航和明确的过期状态，保留日期、原始方案、失败结论、对照实验、尚未解决的验收项。
- 序列化实验 JSON、历史配方、外部副本中的原始路径属于证据，不作全文替换；旧路径可按文件名在索引中检索，或查[旧路径对照](DocumentationPathMap.md)。源码中的历史注释路径保留，本轮不改程序源码。

## 固定路径

根目录七个约束文件保持原位：`ProgramUpdates.md`、`ResourceUpdates.md`、`ReleaseAssets_v1.0.2.md`、`ReleaseAssets_v1.0.2.sha256`、`GameFrameCostAnalysis_20260918.md`、`GameFrameDropAnalysis_20260917.md`、`MapRegionRefactor-Handoff.md`。

此外保留 `OpenWork.md`、`CloudRelease.md`（工具帮助引用）、`CloudReleaseNotes.md`（发布正文）、`CoordinateSampleCapture_zh-Hans.md` / `CoordinateOfflineEvaluation_zh-Hans.md`（采集打包）、`KuroMapSyncInstall.md` / `KuroMapSyncPrivacy.md`（公开安装与商店隐私链接）。这些文档通过索引分类，物理位置保持稳定。

`CloudReleaseNotes.md` 整文件是玩家公告，不得在其页首添加维护备注、目录导航或内部诊断说明。
