# 105 删除位置外推与统一标记投影计划

需求依据：用户明确决定删除遗留外推，修复已确认的 SURF/OCR 显示问题，交付基于上次104包的增量包。此前外推设计由本次指令替代。

## 设计与边界

只使用已经接受的真实定位；显示阶段不估算未来位置，不追赶平滑，不因定位年龄超过2秒清空。保留真实图像位移贴合，以及场景、代际、帧序、质量和可达性保护。删除短窗 PredictAt、影子外推及其回测、OCR predicted-outlier 拦截和备用 Animated OCR 显示实现；历史趋势用于镜像错误识别的质量保护不生成显示位置，继续保留。

普通标记、自定义标记和路线在发布末端统一按该帧接受的位置投影，SURF 与 OCR 使用同一投影入口。保留旧锚点时，标记投影亦使用保留的 pose。500ms新旧锚点连续校正边界目前没有足够触发证据，本次不凭猜测放宽。

## 执行清单

- [x] 先补生产链静态防回归和 SURF 错误中心载荷回归，确认104实现失败。
- [x] App.cpp/App.h/CoordinateTrust.h 删除短窗外推及残留状态；保持OCR后台预热和独立定位提交。
- [x] MinimapCoordinateLayout.h 统一投影；ImageAnchoredOverlay.h 删除 OCR 动画备用路径，迁移显式测试接口；保留真实像素运动。
- [x] 生产链检查、坐标连续性/图像锚定/全套原生回归；审查无外推残留、无绘制年龄淘汰、无保护放宽。
- [x] 版本105、提交源码，完整原生与真实WinUI自包含候选构建，资源/IPC验证。
- [x] 从原始104 ZIP新解压基线，脚本生成104→105差量，重建逐文件differing=0并验证Windows PowerShell5.1。

不改玩家安装目录，不覆盖正在运行的测试树，不发布正式版本。包内必须含resources.pri，自包含由includedFrameworks验证。实机抖动改善由用户新旧电脑验收，自动化验证不代替实机结论。

## 源码验证

- 生产链静态回归在104实现上10项失败，修改后3项测试全部通过；检查真实App发布入口、锚点保留顺序和外推/动画分支清除。
- 新SURF回归及默认OCR直绘回归分别先失败再通过；覆盖错误旧屏幕坐标、全部标记层、保留锚点及真实像素位移仅应用一次。
- CoordinateContinuityTests、ReadoutMotionTests通过，最终含4.9秒保持测试的离线CTest 6/6通过；IMaoOptimizationTests全套通过。
- 独立代码审查发现误删的非外推访问器及失效注释，已恢复/清理并复核；没有剩余重要代码审查项。场景和历史证据访问接口继续保留。
- 删除的旧动画测试验证的是本次明确取消的行为，已由默认直绘、长时间保持与真实像素贴合断言替代；未降低场景/代际/质量保护。

## 交付完成

- 程序源码提交7d5fe8c38f1fbfdd8ee3772a50c6b1fc5ff5860e；增量助手修复提交c930fd5。完整原生及真实WinUI自包含build/publish通过，包内includedFrameworks/resources.pri齐全。候选新编译的OptimizationTests再次全部通过。
- 完整包：out/no-extrapolation-test-20261101-105/program/IMao-v2026.11.1.105-windows-x64.zip；474621134字节；SHA256 47d8cd021b2c984cc16ef69a7dc2076c1666051dfb688fe6be27e0034033ea32。包内资源、隔离IPC、原生picker33断言均通过。
- 最终差量：out/IMao-104-to-105-delta.zip；64465163字节；SHA256 a7145cc53c650205ef126900891117c34b8d3a77b10c178fa014dd6f7e7dbd99。11个程序载荷、5个助手/说明文件。
- 从原始104 ZIP解压基线重建，1477文件differing=0；差量ZIP的16文件逐个校验一致。Windows PowerShell5.1使用实际-File入口且不显式传DeltaRoot，最终VerifyOnly通过。
- 实际-File测试发现旧助手在参数默认值求值时PSScriptRoot为空；改为进入脚本体后解析。新增此启动方式的回归先红后绿，其余错基线/锁定/只读/额外文件保留/目录穿越/联接拒绝测试全部通过。早期out/no-extrapolation-delta-104-to-105.zip仅保留为诊断中间产物，不交付。
- 完全退出104测试客户端并备份后，将最终差量解压覆盖到IMao-WinUI.exe同级，执行_test-delta/Verify-Delta.cmd确认differing=0，再重新启动。实机体验待用户验收。
