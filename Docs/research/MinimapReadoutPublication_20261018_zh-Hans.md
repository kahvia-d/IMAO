# OCR绘制快照发布与SURF搜索状态分离

用户在17:28实机视频后反馈读数投影仍闪烁，授权修复并刷新测试树。证据见主检出 `out/evidence/flicker-video-20261018-172836/findings.md`。

## 根因

初版只绕过绘制端的地形配准。发布端仍将 `CoordinateRecoveryController::Tracking` 作为小地图布局可靠性的判据。新OCR已提交后，更旧的全局SURF失败结果会把共享恢复控制器改成Recovering；发布端随后保留整份旧布局及旧读数证据，直到其500ms期限耗尽。新坐标虽已接受，却没有及时交给绘制端。

第一段中断的最新接受读数实际只有231ms。30段中断中14段能由原始单调截图时刻交叉核对，其中12段最新接受读数只有190–266ms；不能把它们归为读数真的超过500ms。另有真实无候选/二次确认空档，不能声称本次改动消除所有中断。

## 实现

- `AuthorizeReadoutMinimapPublication` 在构造已提交读数证据后、路线观察与旧锚点保留决策前调用。OCR接管时，小地图布局可靠性来自原始已提交fix、区域/代际、帧序、严格小于500ms的时效，以及匹配的HUD中心和比例；背景SURF控制器仍可保持Recovering，不会阻止这份新快照发布。
- 未接管的SURF布局保留原判据；不将OCR视为SURF恢复成功，不改变750ms交回观察。
- 处于OCR接管的待发布布局若不合格，`CanRetainOverlayAnchor`不允许拿旧布局替代。读数原始截图时间始终保留，新截图、发布和绘制不能给它续期。真正过期或区域/代际不符时仍停止绘制。
- 发布已接受新鲜OCR时，状态模型报告tracking/readout-verified，避免同一帧在独立位置已更新时仍被背景搜索写成定位暂留。像素绘制与其他界面状态仍有自己的检查。
- `minimap-readout-publication` 每秒至多一条，包含frame、authorized、fixFrame、fixAgeMs和visualState，便于确认“Recovering但新OCR获准发布”及真正读数过期。绘制端原有独立时效/裁切检查继续生效。

## 验证与边界

`TestAcceptedReadoutSurvivesVisualRecoveryPublication` 使用实际恢复控制器的OnRecognitionFailure、实际旧锚点选择判据、实际绘制放置类，覆盖新OCR→旧全局失败→仍选新快照→旧读数已过期而新布局仍可绘的完整决策链。初版的旧快照被选中和无效读数仍可保留旧锚点两项反例均先红，修复后通过。

同一用例覆盖真过期、未提交、未来截图/帧号、跨区域/代际、未知比例/无效几何、大地图及SURF原策略。此前已有断言不改；不调整OCR权重、符号门槛、读数二次确认或500ms时效。离线测试目标增加链接实际CoordinateRecoveryController实现，不以假状态对象替代。

构建/测试记录在主检出 `out/evidence/minimap-readout-publication-20261018/`；独立只读审阅未发现可复现新问题。交付记录在 `out/evidence/maptest-readout-publication-20261018/`，只有deployment.json及逐文件differing=0才表示测试树刷新完成。未改托管C#或XAML，沿用已发布自包含WinUI及resources.pri，重新编译原生Host。

下一次实机重点核对：新OCR接受后，即便visualState=Recovering，仍出现authorized=1的新frame/fixFrame；绘制端不应继续使用早于这些fix的旧布局。真正无新接受读数的空档仍应按原期限停止绘制，需另外评估其发生频率，不能通过刷新旧时间戳掩盖。
