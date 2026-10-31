# 106 OCR 结果消费延迟诊断计划

需求依据：用户确认 OCR 不加入画面贴合，按已讨论方案排查105日志中的281ms结果待取延迟，交付105→106差异包。

## 范围与设计

105日志证明OCR完成到取走存在长尾，但没有足够证据把延迟全部归咎于某个同步任务。本次先补可归因证据，不凭猜测移动线程、修改质量门禁或削减楼层判断。

- 保留现有completedAt含义（进入结果槽前的时刻）；补计算结束、发布锁等待、取锁开始/取得、出队时刻，均使用steady_clock。
- 定位线程用固定容量阶段环记录捕获读取、上下文处理、OCR取结果/验证/发布、视觉结果消费、楼层判断、请求提交、筛选、发布和等待。只在慢结果时生成详细字符串，最多每秒一条；输出被合并条数及历史未覆盖时长，不能把丢失历史当作零耗时。
- 分别输出异步视觉worker计算耗时和主线程消费/楼层判断耗时。保留旧ageMs含义，增加消费结束年龄，避免用进入消费时刻掩盖之后的同步工作。
- 日志按会话、代际、请求ID、截图帧和单调时钟关联，不依赖秒级墙钟。诊断阈值100ms不是识别/绘制有效期，不丢结果，不隐藏标记。
- OCR保持105直绘；不加贴合、外推或平滑。不确认具体阻塞根因前不改变调度和业务逻辑。
- 阶段记录是墙钟经过时间，包含操作系统调度和日志耗时；主导阶段只能定位当时执行到哪里，不能直接当作该函数的CPU耗时。若取锁早于结果就绪，单独标记此顺序，并让诊断窗口包含这段等待。

## 执行与门禁

- [x] worker时点字段及有界阶段历史；单元回归覆盖281ms阶段归因、部分重叠、等待、环溢出和嵌套恢复。
- [x] App实际生产链分阶段计时，慢请求输出关联证据；补同步楼层分类/消费结束耗时。
- [x] 定向、原生回归和独立审查；确认正常日志自动记录，无须完整录制。
- [x] 版本106、提交代码、完整原生和真实WinUI自包含publish及包内资源/IPC门禁。
- [x] 以原始105ZIP解压基线生成差量，重建1477文件differing=0，校验ZIP和Windows PowerShell5.1真实-File入口。

不改C:/Dapps/IMao，不覆盖运行中测试树，不发布正式版。原始105ZIP SHA256：47d8cd021b2c984cc16ef69a7dc2076c1666051dfb688fe6be27e0034033ea32。

## 源码验证记录

- LocalizationLatencyTraceTests先红后绿；后续增加环溢出后近期窗口仍完整的回归，修正了永久为真的截断标记，复跑通过。
- CoordinateYolo离线CTest 7/7通过；105的生产发布链保护脚本3/3通过；原生CoreHost编译和OptimizationTests通过。
- 独立审查未发现Critical/Important项；确认trace只有定位路径访问、所有早退恢复阶段、没有新增锁或改变原结果槽行为。
- 分阶段日志记录耗时现场，不把阶段墙钟耗时直接称为CPU时间。106是诊断包，尚未确认旧电脑281ms的最终根因。

## 交付完成

- 程序源码bbab795106a0176d94b467ba9b0dc7e3dfb986ed。完整原生及真实WinUI自包含构建/publish通过；包内资源、隔离IPC、picker33断言通过；候选重新编译的OptimizationTests再次全部通过。
- 完整包：out/ocr-delay-test-20261101-106/program/IMao-v2026.11.1.106-windows-x64.zip，474633566字节；SHA256 ae1772c65fcd5c77bef48095373031b2b2a0eba845b8236666d3441bd28769ff。
- 差量：out/IMao-105-to-106-delta.zip，64477588字节；SHA256 0c7f729c73e7a874c1b2ad03822ace3cb59ddde3303d9e0eda7e868afb34a406。11载荷+5助手/说明文件，含resources.pri。
- 原始105基线覆盖重建1477文件differing=0；差量ZIP16文件逐一一致；Windows PowerShell5.1实际-File且无显式DeltaRoot的VerifyOnly通过。
- 使用：完全退出105并备份，解压覆盖到IMao-WinUI.exe同级，运行_test-delta/Verify-Delta.cmd确认differing=0再启动。正常跑图复现后提供普通Logs即可，无须开启完整录制。
- 待实机证据：coordinate-readout-wait中的dominantStage/phases/segments、coordinate-readout-latency中的workerPublishLockWaitMs/readyToTakeStartMs/takeLockWaitMs，以及readout-visual-probe的layerObserveMs/consumeMs/ageAtConsumeEndMs。不能提前将281ms归因于楼层分类。
