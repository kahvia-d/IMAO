# 107：异步楼层分类与 OCR GPU 设置实施计划

目标：消除 106 日志已经证实的同步楼层分类阻塞，并提供设置页 GPU 开关，交付 106→107 自包含差异包。

方案：复用 LatestTaskWorker，仅在后台执行纯楼层特征分类；保持一个在途任务，不排队积压。索引使用不可变共享快照，结果由定位线程提交，检查索引、场景、会话与 UI 代际。保留分类阈值、楼层防抖、开放层及自有纹理规则。同步接口继续供离线测试使用。切换界面或失去定位时取消旧任务。

GPU 设置仅控制已有 OCR DirectML 后端。新增“性能优化”卡片，保存选择并提示完整重启生效，同时展示实际后端及回退原因。未保存选择时兼容 CPU/GPU 测试脚本；显式保存后设置优先。默认仍为 CPU。不增加 OCR 画面贴合、外推或显示超时。

## 实施与验证

- [x] 原生：LayeredMapState 拆出异步分类，App 接入提交/取回/取消；补充后台阻塞、取消、过期代际及索引替换保护测试，运行原有楼层规则测试。
- [x] 托管：RuntimeConfiguration / CoreHostService 持久化并传递 GPU 选择，SettingsPage 新增卡片；验证缺省、保存、脚本覆盖优先级及实际状态展示。
- [x] 全分支独立审查，修复确认问题；运行原生测试、托管测试与真正 WinUI publish。
- [x] 版本 2026.11.1.107，基于原始 106 ZIP 重建差量；包含 resources.pri，确认 includedFrameworks、应用差量后逐文件 differing=0，验证 PS5.1 -File 默认路径。

风险重点：后台不能直接修改楼层状态；运行中索引替换不能悬空；旧场景结果不能重新启用楼层；GPU 请求不代表实际成功；设置保存失败不能呈现成功。

本轮用户已授权实现和交付；沿用当前 codex/minimap-completion-batch 分支执行，不刷新玩家安装树或正在运行的测试树。

## 验证记录

- 新增托管偏好测试先编译失败再通过；ManagedRuntime 全套通过，默认 CPU、未保存时继承启动脚本、显式设置覆盖脚本、序列化保存均覆盖。
- 原生异步接口测试先失败后通过：阻塞分类时主线程仍可返回、拒绝积压、取消、旧代际/场景/索引拒绝、同一结果只消费一次。
- 独立审查发现旧截图与新位置组合的风险，已补来源锚点和逐楼层空间证据比较（Contains 与共享地面阈值）。跨界测试旧实现明确失败 exit=1，修复后通过；同范围内移动仍可采纳，不引入 TTL。
- 楼层规则原有测试通过；105 发布绘制约束脚本 3/3 通过。复审无 Critical/Important。
- 测试脚本说明同步更新：保存过 GPU 偏好后设置优先，无须完整录制，仅 GPU 加速 OCR。

## 107 交付

- 程序提交：4b144db157c66fcc538ad405ed707d435d8e23d3。全新原生构建、真正 WinUI Rebuild + 自包含 publish、核心 OptimizationTests、包内资源/隔离 IPC/原生 picker（33断言）通过。
- 最终楼层测试补充真实 10:10 近似平票：跨范围拒绝，同范围内移动可消费，三次有效票仍正常启用楼层。
- 完整包：`out/ocr-async-test-20261101-107/program/IMao-v2026.11.1.107-windows-x64.zip`，474643437 字节，SHA256 `28c2175b213dcc034895379762a0e6e06d921b84bb7b03d5586b2c0898c66153`。
- 差量包：`out/IMao-106-to-107-delta.zip`，64488684 字节，SHA256 `6a6515b4669bd21b22d0b47f292a6481962f86a467a52f22d67fdd6c46dd2869`。12载荷+5助手说明文件，含 resources.pri。
- 以原始106ZIP新解压基线实际覆盖，还原1477文件 `differing=0`。ZIP17文件逐一校验一致；PS5.1真实 -File 且不显式指定 DeltaRoot 的 VerifyOnly 通过。runtimeconfig 含 includedFrameworks。
- 使用：从托盘完全退出106并备份，解压覆盖原测试目录，运行 `_test-delta/Verify-Delta.cmd` 确认 differing=0 后启动。设置→性能优化可切换 OCR GPU，完整重启生效，保存选择后优先于 CPU/GPU 脚本。
- 尚待旧电脑实测延迟改善，不声称所有滞后已经消失。普通日志新增 layered-floor-async 的 computeMs/submitToConsumeMs/positionEvidenceChanged，结合106的OCR分阶段日志判断实际变化，无须开启完整录制。
