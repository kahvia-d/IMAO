# 2026-09-07 跨区域恢复回归

这些原始截图来自游戏会话 `20260907-211625`，截取时间为北京时间 21:16:27–21:16:45。文件从 `out/minimap-audit-20260907/diagnostics/` 原样复制；每份图片的 SHA-256 写在清单中。没有裁剪、去除覆盖物或补绘。

`consistencyExpected` 是先前排查中由运行时大地图日志、当前截图的附近匹配和几何复核交叉得到的参考坐标，**不是独立人工真值**。此测试可以检验已知故障能否恢复、恢复结果是否与原证据一致，不能测定全地图准确率。诊断报告见 `Docs/MinimapRetrievalAudit_20260907.md`。

## 两个独立场景

- `Tests/VisualLocalization/20260907-cross-area.json` 不提供任何坐标提示。第一张截图允许最多 24 次有界全局恢复请求，必须实际推进到排名靠后的候选区域并找到几何支持；同一截图的重试只用于继续搜索，不能充当第二次确认。下一张独立截图必须完成确认发布，随后八张截图必须通过 `TrackLocal` 连续跟踪。空白与固定种子噪声是负例，其中噪声连续检索八次，覆盖扩大搜索后的误接受风险。
- `Tests/VisualLocalization/20260907-resume-hints.json` 单独验证产品使用的 `MinimapResumePolicy`：原小地图坐标失败两次后，转到新近大地图视口线索；第一张截图即使有强几何支持也不能直接发布，第二张截图才能确认，第三张必须可以继续跟踪。这个场景显式提供搜索提示，不与前一个无提示全局检索结果混算。

两者均调用产品的 `MinimapVisualConfirmation`，并维持现有仿射、内点、覆盖象限等几何约束。

## 清单与报告

顶层 `recoverySequence: true` 选择恢复场景，既有大小地图回放的默认行为不变。`provenance.independentGroundTruth` 必须显式为 `false`，场景不允许使用 `expected` 或 `worldHint` 冒充独立标注或暗中限制全局搜索。

每个 `session` 是独立恢复状态；切换时清除确认、跟踪及旧异步结果。`maxRecoveryFrames` 限制一张截图的搜索重试次数；`maxSearchMilliseconds` 限制单次请求耗时；`maxRecoveryMilliseconds` 限制该截图总耗时。`mustRecover`、`mustPublish`、`mustTrack` 和 `mustDeferPublication` 分别检查几何恢复、确认发布、局部跟踪和首帧禁止发布。`mustReject` 要求不能获得可恢复的几何支持，也不能发布。`consistencyExpected` 仅在候选出现后检查其场景和坐标距离，绝不用于查询。

`requireSearchProgression` 要求实际出现非零检索起点；第一张正例还单独使用 `mustContinueSearch`，避免负例的继续搜索掩盖正例未覆盖该路径。报告记录每次请求的 `coarseSearchOffset`、`verifiedCoarseCandidates`、`nextCoarseSearchOffset`、是否复核上一候选和尝试的旋转角，避免仅凭最终结果推测继续搜索是否发生。

提示场景额外提供 `resumeHints.trusted`、`resumeHints.viewport`，含场景、地图坐标和相对观测年龄；`expectedHintSources` 检查每次实际尝试的提示源顺序。它复用产品的提示队列和两帧确认，不维护另一个仿制策略。

报告使用 `scenarioPassed`，明确输出 `accuracyEvaluated: false`；退出码 0 表示这一故障场景的行为断言通过，退出码 3 表示失败，不能解释成标注准确率通过。真实游戏传送、开关地图以及其他区域仍需另行验收。

```powershell
& ./x64/Release/IMaoVisualRegression.exe "$PWD" Tests/VisualLocalization/20260907-cross-area.json out/minimap-fix-20260907/cross-area.json
& ./x64/Release/IMaoVisualRegression.exe "$PWD" Tests/VisualLocalization/20260907-resume-hints.json out/minimap-fix-20260907/resume-hints.json
```
