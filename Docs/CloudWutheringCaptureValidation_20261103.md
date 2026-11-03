# 云鸣潮捕获验证记录（2026-11-03）

状态：WGC 真实 Edge 捕获、自动排除浏览器栏已验证；产品适配已实现，回归与测试树交付结果见文末。当前真实画面为非探索界面，不能据此宣称定位准确率或大地图对齐已经通过。

## 验证方式

浏览器自动化工具因无法确认当前 URL 停止了操作。改用用户手动触发的独立采样工具，不自动访问、激活或控制浏览器，也不自动选择已打开的云鸣潮窗口。

`IMao-Core/tools/CloudCaptureProbe` 复用已有 `CoordinateOffline/CaptureEngine.cpp` 和 `SampleStore.h`，读取 WGC 的 BGRA 帧并输出同帧坐标条和小地图 PNG。它使用与产品相同的 WGC 技术，但不是产品 App/叠加层的端到端测试；当前使用的是既有独立采样器的客户区映射逻辑。

仅选择 Edge/Chrome。启动后不采样；用户在目标窗口按快捷键才绑定 HWND、PID、进程创建时间。最多 6 组、5 秒、16 MiB PNG；失去前台、最小化或目标身份变化时结束。工具不识别同 HWND 的标签切换，采样期间需保持云鸣潮标签。不得用它自动采集其他网页作绕过验证。

补充：用户反馈全屏游戏可能截获快捷键，并明确要求助手自己启动采样。新增 `--window <十进制HWND> --output <目录>` 模式，直接绑定明确指定的可见 Edge/Chrome 窗口；不弹窗、不注册热键、不激活浏览器，允许被其他窗口遮挡，但最小化/身份失效仍终止。此模式必须在用户授权采样目标后使用；不按进程自动跟随其他窗口。

## 真实云游戏采样结果

2026-11-03 11:59（北京时间），显式窗口模式得到 6 组同帧坐标条/小地图图像，保存于 `out/evidence/cloud-wuthering/direct/cloud-D54A6BED-FE4F-406A-BE25-E8B8A8FA8C2C/`，含 `report.json` 与 `index.jsonl`。

- 捕获客户区为 1920×1032、DPI 96、客户区偏移 `(0,0)`。6 组坐标条和小地图均非全黑。
- 目视检查第 1、6 组：底部为游戏画面与坐标信息，顶部小地图采样包含浏览器工具栏，小地图被裁去一部分；画面随样本变化。
- 两个小区域的测得回读耗时约 0.24–1.04 ms，不包含串流、端到端识别或整幅画面捕获延迟，不能作为产品性能结论。
- 结论：本机当前 Edge 会话可通过 WGC 捕获。当前浏览器客户区不能直接视作游戏视口，既有 HUD 裁剪尚不正确；需要识别/确认实际游戏区域，或在确认的纯游戏全屏状态重新采样。不能据此认定是用户未使用游戏全屏，也不能假定 F11 和网页全屏等价。
- 未验证：YOLO 读数准确率、完整小地图定位、大地图对齐、Chrome、切标签门禁、断流恢复。`recognitionValidated`、`tabIdentityValidated` 继续为 false。

## 本机已验证

- 新目标选择测试先出现 3 项预期失败，实现后 0 失败：Edge/Chrome 精确进程名、大小写、不同窗口、PID/创建时间变化、未选择、隐藏、最小化和失去前台。
- 复用的原生 WGC fixture 通过：暂停期间不写图片；无边框和有边框两个 800×450 客户区各采 3 组；检查坐标条与小地图的精确 BGR 像素、边界映射、两区域来自同一绘制代次。
- fixture 证据：`out/evidence/cloud-wuthering/fixture-20261103-114818/`。只包含自绘测试窗口，不含浏览器页面。
- 初次独立 C++ 探针验证不涉及产品；后续已经修改运行时、托管代码及 XAML，并运行真 WinUI 构建。
- 构建复用本地 OpenCV 和 nlohmann；没有下载、改动玩家安装或刷新测试树。

## 用户手动采样

构建（已有二进制时可跳过）：

```powershell
. .\scripts\Enter-DevEnvironment.ps1
.\scripts\Build-CloudCaptureProbe.ps1 -DependencyRoot C:\Dcode\WWMAP-TOOLS
```

启动：

```powershell
.\scripts\Start-CloudCaptureProbe.ps1
```

1. 阅读并关闭工具提示，手动进入云鸣潮实际游戏画面。普通窗口和全屏都属于产品支持范围。
2. 保持游戏窗口在前台，按 `Ctrl+Alt+F9`，等待最多 5 秒。期间不切标签；`Ctrl+Alt+F10` 取消。两分钟不触发会退出，不创建采样目录。
3. 结果弹窗显示输出目录。`out/evidence/cloud-wuthering/manual/cloud-<随机ID>/` 内包含 `index.jsonl`、`report.json` 和最多 12 张小图。

报告记录帧尺寸、客户区偏移、DPI、回读耗时、全黑检测。`captureProducedSamples=true` 仅表示写出了图片；`recognitionValidated=false` 和 `tabIdentityValidated=false` 明确保留未验证状态。重复图像不能单独证明视频断流，新 WGC 时间戳也不能证明服务器帧新鲜。

## 自动区域验证与后续实测

使用 `--auto-window <十进制HWND> --output <目录>` 可调用产品的 UIA 区域解析器后采样，不需要快捷键或手动框选。2026-11-03 自动采样记录：

- `out/evidence/cloud-wuthering/automatic/cloud-1097E6FE-A9A9-47A6-B5DF-89D6275C95B3/`。
- Edge 客户区 1920×1032，自动区域 `(4,80,1912,948)`，排除了浏览器工具栏。
- 当前为启动/菜单画面，只有 1 组静态画面；未用于 OCR 准确率结论。
- 后续产品增加成对黑边识别，独立探针的 `--auto-window` 目前仅验证 UIA 表面；产品预览及运行时另做像素边界细化。

仍需用户游戏会话实测：Chrome/Edge 探索坐标与小地图、大地图叠加及点击、普通窗口/网页全屏/F11 切换、缩放跨屏、切标签、最小化恢复、云端断流。不可把捕获到图像或 fixture 通过写成上述项目全部通过。

## 本轮产品验证与测试交付

2026-11-03：

- 原生 CoreHost + 真 WinUI 构建通过；6 个页面导航元数据、自包含依赖检查通过。
- `scripts/Test-Runtime.ps1 -OutputDirectory out/cloud-wuthering-runtime -Parallel 4` 完整通过（原生优化、捕获、OCR 依赖、点位/路线/分层、托管服务、资源/程序更新、资源选择器与手柄）。
- `CoordinateContinuityTests` 通过，新增同尺寸不同 geometry revision 拒绝旧异步证据用例。独立启动需将 `x64/Release` 放入 PATH 以加载 OpenCV。
- `Tests/CloudCapture` 通过；最终原生目标测试通过，覆盖缩小帧的越界防护、成对黑边、全黑帧、三帧稳定和过时代次。
- 独立审查发现并修复：异步视觉锚点丢失 geometry revision、地图鼠标原点和手柄几何仍使用浏览器客户区、窗口缩小期间像素读取边界竞态、像素区域更新后定位重置延迟。修复后再次构建并运行目标测试和产品冒烟。
- 产品 CoreHost 预览返回 1912×948，约 576 ms；窗口范围 IPC 同时返回原客户区和游戏视口 `(4,80)-(1916,1028)`；错误进程创建时间被拒绝，显式 start/stop 请求被接受。这是接口与捕获验证，不是探索识别验证。
- 自包含 `dotnet publish` 完成。`resources.pri` 具有 `App.xbf`、`MainWindow.xbf`，runtimeconfig 具有 `includedFrameworks`。
- 检测测试客户端已完全退出后，使用 `Refresh-MapTestBinaries.ps1` 刷新 `C:\Dcode\WWMAP-TOOLS\out\map-test`；373 个载荷文件逐个 SHA256 比对，`differing=0`。随后重新启动，WinUI 主窗口和 CoreHost 均存活响应。
- 证据：`out/evidence/cloud-wuthering/product/report.json`、`deployment.json`；构建与回归日志位于 `.superpowers/sdd/CloudWutheringAdaptationPlan_20261103/` 和 `out/cloud-wuthering-runtime/`。玩家安装未改动。

使用说明见[云鸣潮网页版使用与测试](CloudWuthering_zh-Hans.md)。真实 Chrome、探索准确率及全屏切换矩阵仍需后续测试，未标为通过。

## 选择窗口导致核心断开：修复记录

用户实测发现选中 Edge 后 `CoreHost 已断开`。根因不是 WGC 崩溃：960 宽 PNG 为 886251 字节，Base64 后超过 `CoreHostService` 既有 1 MiB 单条消息限制，接收循环主动终止。最初用 PowerShell 直接读取管道的冒烟绕过了托管接收限制，因此漏检。

修复：`BrowserPreview::Encode` 限制双边尺寸、移除不需要的 alpha，并按实际 PNG 字节数缩小，保证 PNG 不超过 512 KiB，为 Base64 与 JSON 信封留足余量。客户端消息上限保持不变，游戏区域尺寸与定位输入不受预览缩小影响。

回归：新增 `ManagedRuntime cloud-preview <host-directory> <browser-pid>`，使用产品 `CoreHostService` 连续预览并发送后续配置命令。修复前明确复现 `IOException: CoreHost 已断开`；修复后连续三次成功（445151 字节 PNG，1912×948 原始视口）。独立原生测试覆盖 1920×1080、3840×2160 随机噪声及 640×12000 长图，编码均可解码且消息预算小于 1 MiB。原生与真 WinUI 构建通过。

检测测试客户端退出后，以完整既有自包含载荷加入新核心，使用刷新脚本更新测试树；373 个文件核对 `differing=0`，重新启动。证据：`preview-red.log`、`preview-green.log`、`preview-fix-build.log` 和 `out/evidence/cloud-wuthering/product/preview-fix-deployment.json`。

## 小地图无定位：测试树模型联接失效

2026-11-03 用户反馈大地图正常、小地图不稳定。日志在 12:54:10 明确记录 `ocr-initialize ready=0 error=coordinate YOLO runtime DLL unavailable: 126`；13:26–13:28 有预加载请求但没有 OCR 提交，`ocr-bounded` 搜索提示实际来自楼层图像匹配，并非文本识别成功。大地图两次确认玩家坐标，而小地图视觉恢复持续失败。

实物检查：测试树 `Assets/models` 是指向旧诊断 payload 的 junction，`coordinate-yolo` 已空。程序文件哈希一致不足以保证识别资源可用。

修复：`Refresh-MapTestBinaries.ps1` 调用 `MapTestModelPayload.ps1` 检查 YOLO 模型、CPU/DirectML 运行库；仅解除测试目录的联接本身，保留旧目标，复制完整模型资源为测试树自有目录并逐文件核对。模型不再依赖可清理的诊断产物。`Test-MapTestModelPayload.ps1` 覆盖 dry-run 不改目录、解除联接不写外部目标、重复同步、缺失资源拒绝。

实际部署对照：同一 OCR 依赖/推理测试对修复前测试树复现错误 126；刷新 25 个模型文件后通过。全套程序载荷和 25 个模型文件重新核对一致，再启动测试客户端。旧玩家安装未改动，无下载。随后短时采样只有灰色静态画面，因此没有把依赖修复宣称为真实云游戏识别准确率验证；需要回到探索画面再试。
