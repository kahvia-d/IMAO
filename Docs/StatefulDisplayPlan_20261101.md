# 104 状态式显示与位置保持

需求依据：用户本轮明确要求修改两项行为后交付103差量包。此前250ms许可/500ms位置绘制期的设计在本轮被替代，不放宽识别质量、场景和顺序检查。

## 实施与验证

- [x] 显示许可按检测出的界面状态保持；保留界面漏识防抖、切换/失焦撤销，健康看门狗独立从检测处理完成时间计时（2秒），不再沿用截图250ms年龄。重复帧不得续健康，同帧状态机修正允许但不续期。
- [x] 去除OCR及显示布局的固定绘制年龄淘汰；保留捕获健康、未来时间、场景、generation、帧序检查。Fresh仍用于交互/自动操作，不等同Drawable。
- [x] 慢OCR/视觉识别结果可以消费，但不能覆盖更新的位置；跳变预算与趋势按原始采集时间计算，处理延迟不扩大移动预算。
- [x] 没有新位置时保留会话内接受的位置；SURF位置与原始图像锚点成对保留，不把旧位置伪装为新帧测量，不跨场景/界面恢复信任。
- [x] 针对新行为的红绿回归、完整原生测试及独立审查；完整自包含构建并验证资源/IPC。
- [x] 从原始103 ZIP重新解压基线，生成104差量；逐文件重建、PS5.1和ZIP校验，再交付。

## 交付边界

保持旧位置可能造成暂时滞后，符合用户本轮选择。连续无有效候选的识别原因没有在本轮改模型。真实界面切换/失焦/捕获断流仍可隐藏。检测线程无完成结果超过2秒属于独立健康故障，不是坐标绘制期限。玩家安装目录不修改，不发布正式版本。

## 验证记录

- 新的慢结果、长时间位置保持、独立检测健康和同帧状态修正测试先红后绿。PositionDisplayTests、StateVisibilityTests及全套OptimizationTests通过。
- CoordinateHandoffTests、ReadoutMotionTests、CoordinateContinuityTests通过；覆盖场景/代际/未来时间/顺序拒绝及相同锚点复制发布后的连续移动。
- 复审修正异步SURF残余250ms淘汰、同帧状态修正、处理延迟放大跳变预算，以及重复发布重置图像跟踪的问题；最终静态复审无Important/Critical遗留。
- 既有要求按年龄隐藏的测试仅按本轮用户明确的新要求迁移；跨场景、未来时间、截图健康和交互新鲜度断言保留。

## 交付完成

- 源码：a0963814a9ab25f22dce3eb6f21a88612b4cc203，版本2026.11.1.104。原生全量构建、真实WinUI自包含publish、完整资源/IPC及picker33断言通过。
- 完整包：out/stateful-test-20261101-104/program/IMao-v2026.11.1.104-windows-x64.zip，474630393字节，SHA256 1e58dfc99917c2daafcbd1d1ea2d8cc3f93f1c1b2047053fd55c283591890281。1477文件、0联接、includedFrameworks和resources.pri齐全；ZIP逐文件differing=0。
- 差量：out/stateful-delta-103-to-104.zip，64474408字节，SHA256 fdf2cdc73cbd3fd796af417fb5e3bc3f1bc41a3ae57015f9cf1f1761d4bff475。11个程序载荷+5个校验/说明文件。
- 基线为原103完整ZIP（efc5230f3c6ea502e2959e5d34e821dca8f9948eb49600db910afb6837b5b694）重新解压副本。实际应用差量后1477目标文件differing=0；Windows PowerShell5.1 VerifyOnly通过；差量ZIP16文件逐一一致。
- 先完全退出IMao并备份103测试目录，再将差量内容覆盖到IMao-WinUI.exe同级，运行_test-delta/Verify-Delta.cmd确认differing=0后重启。实际旧电脑体验仍待用户验收。