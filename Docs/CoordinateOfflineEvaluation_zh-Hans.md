# 坐标 OCR 独立离线评测

2026-10-14。目的：用已有真实截图比较 IMao 的 PP-OCRv5 和网友项目的单字符 YOLO，
给后续模型选择提供可复现的依据。评测不改变生产取景、识别调度、搜索提示或位置发布。

## 范围与素材

`Tests/CoordinateOffline/corpus.json` 登记 17 张原始截图：

- 11 张历史人工判读的完整三维坐标，标注沿用 `chore/coord-ocr-vs-yolo` 最后提交归档的清单。
- 草地难例 1 张，只有 x=229、y=504 是已确认标注；z 保持未知。
- 雪地难例 1 张，仅观察输出，没有经过独立确认的标签，不进入准确率分母。
- 4 张地图 UI 的无读数截图；此次检查了左下角区域，只作为无读数反例。

历史截图已被反复分析和调参，属于回归集。旧参考帧的会话边界不完整，
不能宣称它们构成独立留出集，也不能凭 4 张无读数图证明实机误报率。
上一轮混合整块截图的 60 张合成半透明图不进入正式计分。

源图片、Paddle/OpenCV 和上游模型使用本机已有文件，由 `--data-root` / `--upstream` 指定。
代码不下载仓库、模型或依赖；文件缺失会失败并指出原因。

## 输入隔离

Python 编排器从同一原图生成无缩放、无增强的 BGR PNG 裁剪，两侧读取同一文件。
记录原图 SHA256、裁剪文件 SHA256、尺寸和矩形；两侧分别计算解码像素的 FNV-1a，逐条一致才计分。
三种区域预先固定，使用客户区左侧/底部锚定：

| profile | 区域 |
|---|---|
| production | 1600×900 参考坐标 `(20,865)-(160,900)`，等于当前 IMao 取景 |
| readout-wide | 参考坐标 `(20,870)-(248,900)`，仅作为历史研究取景的诊断对照 |
| upstream-band | 左侧四分之一宽，贴底高度 `floor(H/32)`，来自上游的默认区域规则 |

每个引擎只收到 `inputs.json`：图片、ID、区域与摘要，没有标签、前一帧位置或标注真值。
每张图独立识别；不把上一条标注灌给下一条。

## 实际代码复用

原生 `IMaoCoordinateOffline` 使用当前项目的 `CRNNRecognizer` 和 `CoordinateCandidateParser.cpp`。
CMake 从 `IdentifyWorldCoordinates.cpp` 的固定边界提取并编译实际 `PreprocessCoordinateRegion` 函数体，
没有在 Python 中重新实现预处理。提取边界变化会使配置失败。
这个做法让评测独立取景，同时无需给生产请求增加 ROI 开关或改动运行时代码。

原生构建嵌入预处理/解析器/适配器源码、直接相关头文件、`ppocr.lib` 和构建配置的摘要。
运行前后核对文件摘要，读取原生报告后核对编译时摘要；源码或库变了却没重建会失败。
报告同时记录模型、字典、评测脚本、上游代码与提交、工具版本、编译器和线程数。

YOLO 适配器加载本机上游 `src/ocr_engine.py` 的实际类和函数，仅为 Qt 和路径提供桩。
使用原 ONNX 模型、字典、检测和 spacing-dominant 解析；不重新实现检测或解析算法。
上游会捕获推理及解析异常，因此适配器检查其 ERROR 日志及解析失败的 DEBUG 日志，
包含预热阶段；异常导致整轮失败，不能被当成正常无输出。
这条日志签名针对报告记录的本地上游版本；升级上游时必须重新验证异常探针。

## 四条管线与指标

- `ppocr-contrast` / `ppocr-tophat`：分别用当前实际预处理路线，解析后取最高模型分数的候选，
  同分保留解析器顺序。这是固定的离线 top-1 规则。
- `ppocr-corroborated`：调用当前实际 `Corroborate`，TopHat 为主、contrast 作证，要求 XY 完全一致，
  观察没有新鲜先验时的识别互证效果。
- `yolo`：只计上游解析器实际接受的单条坐标。

XY 和 XYZ 命中、候选召回、错误返回、无输出、负号错误、无读数误报分别保留。
未知 z 不进入 XYZ 分母；未知真值的难例只显示输出；正确答案出现在较低候选位置不会让 top-1 命中。
z 错误与 XY 位置错误分别计数。

这里没有模拟 App 的位移预算、视觉匹配、连续帧确认或最终位置发布，
因此报告中的“正确输出”不等于“正式定位成功”，不能用它决定放宽发布门禁。

两侧都用 CPU，推理线程设为 2–4（与当前 IMao 初始化规则一致），YOLO inter-op 为 1。
先用空图预热，每个输入重复 3 次；各帧取中位数，再计算整个 profile 的 P50/P95。
耗时包含预处理、推理和解析；不含读盘、像素摘要、模型初始化。初始化耗时单列。
原生 OpenCV 与 Python OpenCV 的版本单列，不能把速度差全部归因于模型架构。
重复运行的文本或坐标不一致会失败；性能数值仍会受本机负载影响。

## 运行

在包含这套代码的工作树中运行。独立工作树可以复用原工作区的依赖和图片：

```powershell
$dataRoot = 'C:\Dcode\WWMAP-TOOLS'
. (Join-Path $dataRoot 'scripts\Enter-DevEnvironment.ps1')
$python = Join-Path $dataRoot 'out\coord-ocr-compare\.venv\Scripts\python.exe'
$upstream = Join-Path $dataRoot 'third_party\research\WutheringWaves-Navigator'

# 只构建独立原生评测器，不编译/刷新测试客户端，不复制玩家版安装文件。
.\scripts\Build-CoordinateOffline.ps1 -DependencyRoot $dataRoot

# 23 项检查，包括对上游真实代码注入故障；不下载测试依赖。
$env:IMAO_OCR_UPSTREAM = $upstream
& $python -m unittest discover -s Tests\CoordinateOffline -v

# 每轮使用新目录，已含 report.json 的目录会被拒绝，保留旧证据。
$output = Join-Path $dataRoot ('out\coordinate-offline\run-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
& $python tools\coordinate-ocr\benchmark.py `
    --data-root $dataRoot --upstream $upstream `
    --native out\build\coordinate-offline\IMaoCoordinateOffline.exe `
    --output $output --repeats 3
```

输出目录含 `report.md`、完整 `report.json`、两侧原始报告、`inputs.json`、全部实际裁剪和原生日志。
非零退出表示该轮没有有效完整结论；输入、引擎或摘要异常不能靠忽略样本放行。

## 本轮观察与下一步

同一份现有回归集，单条输出结果可复现：

| 输入与管线 | XY 命中 | XYZ 命中 |
|---|---:|---:|
| production / PP-OCR contrast | 10/12 | 7/11 |
| production / PP-OCR TopHat | 8/12 | 5/11 |
| production / PP-OCR 双路线互证 | 7/12 | 4/11 |
| production / YOLO | 11/12 | 7/11 |
| upstream-band / YOLO | 12/12 | 11/11 |

四张无读数图中，所有管线都没有输出坐标。YOLO 能读出草地难例的 XY，z 没有可信标签。
YOLO 在 readout-wide 下把晋州帧的 x=-96 读成 +96，说明取景改变仍会引入负号错误。
这批素材支持把专用 YOLO 作为下一轮候选；它不支持直接出货或宣布已解决半透明场景。

下一阶段需要新会话的真实素材，按会话划分评测，覆盖复杂明亮背景、特效、传送残缺读数、
没有读数的画面和不同客户区尺寸。先以旁路记录测恢复收益和错误位置风险，再讨论正式接入或训练。
