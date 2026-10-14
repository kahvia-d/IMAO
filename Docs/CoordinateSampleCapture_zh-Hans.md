# 坐标识别素材采集

## 使用

工具独立运行。第一次建议采集 3–5 分钟正常游玩，包含移动、明亮草地/雪地、特效、
传送、打开地图或菜单。没有坐标的画面同样有用，不需要手动挑图。

1. 打开工具包内的 `Start-Capture.cmd`。启动后处于暂停状态。
2. 切回《鸣潮》，按 **Ctrl+Alt+F8** 开始自动采集。
3. 同一快捷键可暂停/继续；切到其他程序时也会停止采样，回到游戏后继续。
4. 按 **Ctrl+Alt+F7** 结束并保存索引；也可打开 `Stop-Capture.cmd` 正常结束。

默认每秒 2 对。第一次开始后最多运行 10 分钟，或达到 1200 对、256 MiB PNG 数据时结束。
字节预算合计两张图；不足以保存一整对时，两张都不写入。
时间上限包括暂停及切出游戏的时间。没有运行 OCR，不会把任何读数送入定位。

素材默认保存在工具包旁边的 `coordinate-samples/session-<ID>/`，每次启动新建独立会话。
启动窗口会显示完整目录；诊断写在 `launcher-*.stderr.log` / `launcher-*.stdout.log`。
快捷键被其他程序占用、无法访问游戏、捕获异常和几何不匹配会明确报告。

## 保存内容

- 每次采样保存两张无损 PNG：`frame-000001.png` 是左下角原始像素带，
  `frame-000001.minimap.png` 是同帧小地图。
- `index.jsonl`：一条样本记录关联两张图，共用样本 ID、原客户区尺寸和原始帧时间；
  两侧分别记录客户区裁剪矩形和 PNG 摘要，以及采样/暂停状态和结束原因。
- 每个会话有独立 ID，用于后续按会话划分数据，避免相邻帧泄漏到训练与测试两边。

取景仅用于保存素材：宽为 `max(W/4,400×HUD scale)`，高为 `max(H/32,80×HUD scale)`，
锚定客户区左下角；不缩放、不增强、不按是否含有坐标筛选。只有前台
`Client-Win64-Shipping.exe` 窗口会被采集；没有键鼠输入注入。

小地图取景沿用项目 `HudLayout.h`：1600×900 参考矩形为 `(30,23)-(184,177)`，
按统一 HUD 比例缩放并锚定左上角。保存原始像素，不归一化为模型输入尺寸；
地图或菜单打开时仍保存这个位置的画面，不根据匹配是否成功筛选素材。

Windows Graphics Capture 绑定游戏 HWND；同一个捕获帧纹理中分别回读坐标带和小地图，
没有为两块图分别发起截图或使用异步 App 坐标。两个区域齐备后才写入配对索引。
支持时使用生产端 `MinUpdateInterval` 限速，其他系统仍在消费端限制回读频率。
窗口尺寸改变时更新帧池；客户区映射不能精确确认时拒绝该帧并记录诊断。
捕获格式为 BGRA8，保存时仅去除 alpha 通道，保留 BGR 值；没有为 HDR 做额外色调处理。

## 构建与独立工具包

在独立工作树中运行，复用已有依赖，不下载内容，也不刷新客户端：

```powershell
$dataRoot = 'C:\Dcode\WWMAP-TOOLS'
. (Join-Path $dataRoot 'scripts\Enter-DevEnvironment.ps1')
.\scripts\Package-CoordinateSampleCapture.ps1 `
    -DependencyRoot $dataRoot `
    -OutputDirectory "$dataRoot\out\coordinate-sample-tool-paired-20261014"
```

输出目录必须是新的。脚本构建独立原生工具，并复制 OpenCV 及本机已有 VC 运行库，
逐文件核对 SHA256；`package-manifest.json` 登记全部载荷。
工具不依赖 Python、.NET 或玩家版安装来采集；Windows 系统 API 仍由系统提供。
Windows HWND 捕获入口要求 Windows 10 1903 或更高，见
[Microsoft CreateForWindow 文档](https://learn.microsoft.com/en-us/windows/win32/api/windows.graphics.capture.interop/nf-windows-graphics-capture-interop-igraphicscaptureiteminterop-createforwindow)。

直接从 PowerShell 启动或结束也可以：

```powershell
.\scripts\Start-CoordinateSampleCapture.ps1 -ToolRoot "$dataRoot\out\coordinate-sample-tool-paired-20261014"
.\scripts\Start-CoordinateSampleCapture.ps1 -ToolRoot "$dataRoot\out\coordinate-sample-tool-paired-20261014" -Stop
```

启动脚本在后台运行工具，等待它报告就绪；结束脚本核对 PID、启动时间及程序路径，
发送正常结束事件并等待索引写完，检查退出码和完成标记，不强制终止进程。

## 检查与回放

采集结束后，先检查会话。这里才需要原工作区已有的 Python 环境：

```powershell
. (Join-Path $dataRoot 'scripts\Enter-DevEnvironment.ps1')
$python = "$dataRoot\out\coord-ocr-compare\.venv\Scripts\python.exe"
& $python tools\coordinate-ocr\sessions.py <会话目录> --output <新的检查目录>
```

检查器核对每个 PNG 的 SHA256、尺寸、源矩形、重复 ID、结束计数和会话边界，报告全黑图、不同图片数及捕获诊断。
新版索引为 formatVersion=2，要求每条样本都有匹配帧 ID 的小地图，
结束字节计数涵盖两张 PNG。旧版 formatVersion=1 单图会话继续可读，`pairedSamples` 为 0。
缺失或损坏的小地图即使在恢复模式下也不会被忽略；未索引的半对文件仅报告，不导入。
检查输出的 `corpus.json` 可直接用于独立离线评测，保留原客户区几何，不会把像素带误当成小整屏再次取景。
导出保留每帧的 `minimap` 路径、摘要、矩形及共同源帧时间，供后续独立小地图匹配使用。
OCR 回放会重新核对配对文件，但传给两种 OCR 的输入仍只有坐标带裁剪，不含小地图或标签。
初始标签全部未知；回放结果只用于观察，不能把模型预测当成真值。

本工具只采集配对证据，没有生成小地图坐标标签。后续标注应先在人工可读的样本上
核验小地图到游戏 XY 的误差，记录地图场景和匹配质量，并禁用 OCR 提示避免循环验证；
小地图不提供高度 z，无法确认的分量保持未知。

明确的人工标注可通过 `--labels labels.json` 提供，例如：

```json
{
  "session-xxx-000001": {"state":"present", "expected":{"x":229,"y":504,"z":null}, "note":"XY 清楚，z 未确认"},
  "session-xxx-000002": {"state":"absent", "expected":null}
}
```

完整回放命令与口径见 [`CoordinateOfflineEvaluation_zh-Hans.md`](CoordinateOfflineEvaluation_zh-Hans.md)。
新会话应该先作为独立验收素材；在选择模型或参数后才考虑把其他会话用于训练。

## 验证与限制

原生测试覆盖底部锚定、边框偏移、前台条件、无损 PNG、张数/字节预算和已有会话保护。
WGC 集成测试只创建受控的本进程窗口，验证有边框/无边框、暂停时无写入及已知 BGR 像素。
测试会话标成 `fixture`，默认禁止导入真实游戏素材集。
本轮 55 项 Python 检查通过；启动/结束测试使用 PowerShell 5 和原生命名事件，包含异常退出。
原生测试还覆盖双图合计预算和第二张写入失败；WGC 测试在上下两处绘制逐帧变色标记，
验证有边框/无边框窗口的三对截图均来自相同绘制代次。
工具包清除开发环境 PATH 后，通过暂停启动/正常结束、中文目录、零图片写入及逐文件摘要核对。
真实游戏的兼容性及性能影响仍需第一次短会话确认；检查时先看数张 PNG 是否清楚、是否完整。

未正常结束的索引默认报错。需要恢复已落盘的样本时，可显式使用 `--allow-incomplete`；
检查器仍核对图像，报告会记录恢复状态及未索引文件。硬终止导致的未索引文件不具备完整时间/几何证据，不导入评测。
