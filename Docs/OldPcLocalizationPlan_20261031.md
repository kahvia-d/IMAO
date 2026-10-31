# 旧电脑定位与录制隔离测试计划

## 授权与边界

用户已授权实施并要求交付自包含测试包。基线为 `codex/minimap-completion-batch` / `7d86b90`。本次不发布、不改玩家安装树，不放宽 OCR 字符判断，不延长绘制位置的 500 ms 信任期。

## 证据

旧电脑 2026.10.31.101 录制完整，438 张图像、零缺失；采集发布曾间隔 663 ms。250 ms 画面失效被当成 UI 切换，销毁 OCR generation；26 次 OCR 消费全部过期。录制前也有 547 ms 才消费的读数：可作恢复证据，但已超过 500 ms 绘制期。SURF 间歇成功，不能归结为始终无法匹配。29 张 OCR 回放没有观察到接受的错误 XY，7 张拒识暂不修改模型。

## 实施顺序与验收

- [x] 提取画面新鲜度与上下文存活的独立策略。250 ms 后仍隐藏过期覆盖层；在有限的 750 ms 捕获间隙内保留定位上下文，真正 UI 切换、失焦和长时间断帧仍失效。回归测试覆盖边界、未来时间和空帧。
- [x] 允许在短暂捕获间隙消费已完成 OCR，禁止在过期帧上发起新视觉/OCR工作；保留原始 capturedAt。日志区分恢复证据与可绘制时效，不把 750 ms 接受窗口误当成 500 ms 绘制授权。
- [x] 对确认不可变且独立拥有的 WGC 图像提供录制共享所有权入口，消除关键路径的整帧复制。普通调用仍复制；共享入口校验连续内存和独立分配，预算继续按完整像素计费。验证生命周期、释放调用方后仍可保存、队列边界、停止与重启隔离。
- [x] 增加显式选择的 DirectML OCR 试验后端，默认 CPU。独立 DLL 目录，保留现有 CPU 运行时；GPU 初始化或推理失败后只回退一次，记录实际后端和原因。遵守 DirectML 顺序执行及禁用 memory pattern 要求，不改变模型、阈值与 SURF。测试包使用 CPU/GPU 启动脚本选择，暂不增加正式设置项。提供适配器编号选择，不声称默认适配器一定最快。
- [x] 下载前核对大小：官方 Microsoft.ML.OnnxRuntime.DirectML 1.24.4 NuGet 为 12,458,649 字节，低于 100 MB；保留来源、许可证和摘要。测试真实 GPU 推理、CPU 对照、坏适配器回退；对旧电脑 29 样本比较接受结果。
- [x] 运行相关原生测试及实际 WinUI 自包含发布；独立审查整个差异，修复发现的问题。版本递增至 2026.10.31.102，干净提交后用 Build-ReleaseCandidate 整树构建新测试包。检查 includedFrameworks、resources.pri、无联接、逐文件一致。

## 旧电脑验收

同一场景依次 CPU 不录制、CPU 录制、GPU 不录制、GPU 录制，每种运行约一分钟；比较小地图标记、状态栏、OCR 延迟和游戏帧时间。GPU 争用/旧驱动兼容性须实机确认，不以本机成功代替。若 GPU 失败，应自动转 CPU 并留下原因。完整录制仍有吞吐上限，不能宣称永不溢出。

## 执行记录

- 画面间隙回归：旧策略在 690 ms 用例失败；新策略通过。新鲜度仍为 250 ms，位置绘制仍为 500 ms。
- 独立审查发现短暂失焦未锁存的问题；增加失焦→回焦→无新帧时间线，先观察失败，再让 `CanService` 拒绝 pending invalidation，状态线程在失焦时锁存 suspension。
- 发现录制压缩线程原先 NORMAL 高于 OCR 的 BELOW_NORMAL；回归测试先失败，现将录制 writer/四个压缩 worker 设 LOWEST，设置失败显式报告。代价：争用时录制可能更早触及队列上限，不能静默丢帧。
- `ImageShared` 保留完整独立分配，其他视图仍 clone；所有权、队列、停止/重启与优先级测试通过。
- 本机 DirectML adapter 0 完成 29 张旧电脑样本回放，接受 XY/拒识与 CPU 29/29 一致。错误适配器 999999 自动回退 CPU；驱动本地代码页错误字符串统一转 UTF-8，避免写 JSON 再次失败。
- 官方运行时合计下载约 21.8 MB：ORT ZIP 12,458,649 字节，DirectML 范围读取 9,342,072 字节。未下载 202 MB 完整 DirectML 包。
- 原生 CoreHost 构建及 7 项针对性测试已通过。真实旧电脑游戏帧时间和连续游玩效果仍须用户对照验证。
- 复审确认失焦修复与录制优先级处理无 Important/Critical 遗留；失焦回归是服务门禁单测，未模拟完整 App 多线程实机时序。
- 自包含包：`out/oldpc-test-20261031-102/program/IMao-v2026.10.31.102-windows-x64.zip`，源码 `bed055e0b46fb07659e287e85e7bbda85669157d`。
- 真 WinUI rebuild/publish、启动器 self-check、资源/IPC 检查、33 项原生选择器断言均通过。已有 ResourceSnapshotService.cs 的 CS8604 警告仍存在，不属于本次改动。
- 归档验证：1476 文件，differing=0，0 reparse point，includedFrameworks 与 resources.pri 齐全，版本/快照一致；474,623,930 字节。
- SHA256：`2cd2c3a0038d3213c208d92329a9f433bf1607cccbefeab8f477dbf87c7f67a8`。
- 使用最终包内 DirectML DLL 再回放 29 张，实际 backend=directml、无 fallback，与 CPU 接受 XY/拒识 29/29 一致。GPU 推理中途设备失效的回退分支已审阅，但未故意制造设备掉线；已实测初始化失败回退。
- 包含 Start-CPU.cmd、Start-GPU.cmd、OCR-Test-Readme.txt。未 push、未正式发布、未修改玩家安装目录。
