# 坐标文字行、等比缩放与日期边界：离线实验

## 范围与不变量

在 `codex/coordinate-offline-evaluation` 工作树继续实验。正式程序、生产预处理函数、
`CoordinateCandidateParser`、0.65 分数阈值、`Corroborate`、位置发布与跳变门禁均未修改。
使用已有模型和 2026-10-14 的同一采集会话，无新下载、无训练。

开发核验集为 20 张人工 XYZ、16 张确认无读数，以及 109、995 两张只有负号证据的 UNKNOWN。
两张 UNKNOWN 不进入完整坐标评分。其余全量帧也不因返回读数而获得标签。
所有裁切、缩放与文本边界处理都不读取人工坐标或小地图候选。

## 实验分解

1. 原有 production、readout-wide、upstream-band 保持默认定义。
2. 增加 readout-line：1600×900 参考客户区 `(20,878)-(400,899)`，收紧上下背景，
   保留整行、时间戳和右侧文字；当前素材实际裁切为 `(32,1405,608,33)`。
3. `--preprocessing aspect` 复用生产灰度、CLAHE、TopHat，唯一几何变化是移除 312 宽上限；
   另加保留 BGR 的 raw 路线。左右仍各补 4 像素，宽度超过测试模型范围时明确失败。
4. `*-clock` 为独立的日期边界对照。只有三个已识别整数后的空格、完整日期及完整时分秒
   符合格式且公历日期合法时，才把其前缀交给原解析器。没有该证据时仍调用原解析器，
   所以不能说“所有坏时钟都强制拒绝”。不补负号、逗号、数字，不降低置信度。
5. clock 互证仍以 TopHat 为主、contrast 为见证，调用原有 `Corroborate`，要求相同 XY。

`PreprocessCoordinateRegionAspect` 由 CMake 从真实生产函数生成，仅替换函数名和宽上限；
提取边界或宽上限表达式变化时构建失败。新增头文件列入原生编译摘要。
`preprocessingTrace` 记录传给识别器包装层的尺寸和像素摘要；没有对 `Run` 内实际张量插桩。
链接库中的 `CrnnResizeImg` 单独实测可把 584×48 输入保持为 584×48。

## 开发核验结果

每个模型、预处理、取景组合在 38 张核验素材上重复三次。以下均为同一批 20 张 XYZ：

| 模型 / 路线 | XYZ 命中 | 已标注 XYZ 错误返回 |
|---|---:|---:|
| 当前模型，production，原互证 | 11/20 | 2 |
| 英文模型，readout-wide，原 contrast | 18/20 | 2 |
| 英文模型，readout-wide，等比 raw | 19/20 | 1 |
| 当前模型，readout-line，等比 TopHat + clock | 16/20 | 0 |
| 英文模型，readout-line，等比 contrast + clock | 19/20 | 0 |
| 英文模型，readout-line，等比双路线互证 + clock | 17/20 | 0 |

各路线在 16 张确认无读数上均无误报。6 张无读数是事后发现的地图界面反例，
不能把此比例解释为无偏误报率。所有素材来自同一会话，没有独立验收或跨分辨率验证。

英文 raw / readout-wide 仍把 1100 的 y=-5388 读成 -5338。
更紧文字行的 clock 路线在该帧没有输出，原因包含时钟识别不满足边界规则；
这不表示已经解决 8/3 字形混淆。
单路线在 109 仍能把 x 的负号漏掉，输出 +541；互证路线在 109、995 均无输出。
不能把拒绝输出描述为识别出了负号，更不能依赖置信度高就修正符号。

固定源码和参数后的英文全量复核完成 1160 帧、2320 个输入、20880 条管线输出。
608 条核验样本解码文本及所选坐标同三次回放一致，模型、源码和输入摘要校验通过。
这不意味着 1160 帧都有正确坐标标签。

后验检查旧小地图的 437 条近似候选时，256 条有新互证输出，其中一条相距超过复核阈值 8。
第 814 帧原图经独立复核可见 x 前负号，但两路都漏掉并互证输出 +575，分数约 0.964。
同帧 raw + clock 读出 -575，说明保留颜色与灰度两路的行为存在有用差异；
后续可把 raw 额外见证作为独立实验，本轮没有看到反例后再增设规则来改写成绩。
只新增该帧的事后 x 符号证据，完整坐标仍保持 UNKNOWN，不改 20 张 XYZ 分母。
小地图仅帮助发现待复核分歧，确认负号来自原始截图，不是浮点匹配值。
因此，“标注内错误返回为 0”不能解释为整会话无错误；当前候选仍不适合直接替换默认管线。

## 被否决的像素间隔法

最初试验名 coordinate-line，最终命令名改为 `gap-probe`，明确为失败诊断项，
不进入候选全量复核，也不进入默认 profile 列表。代码和失败证据保留以复现问题。

该方法用近灰色局部亮笔画和连通组件寻找横向空隙；透明文字与背景叠加后，
掩码会漏掉真实笔画，因此大空隙不等于时间戳边界。
第 60 帧真实文字 `-778,1420,34`，却选取 `[140,175]` 空隙并截到宽 157，丢掉逗号/z。
通过合成负号与空隙测试不等于真实画面可用。没有按标签调阈值来掩盖失败。

## 验证与耗时解释

71 项 Python 测试通过；两套新原生契约测试通过，覆盖几何尺寸、细负号、超宽拒绝、
真实库缩放、非法日期、坐标缺项、z 与年份粘连、溢出和原置信度门禁。
把 clock helper 临时回退为不提取前缀时，测试在合法日期案例明确变红，恢复后通过。
中途曾因测试目标缺少依赖 include 而构建失败，另一次因恢复文件保留旧时间戳导致
测试对象未重编译；日志保留，补依赖并更新依赖时间戳后重新构建验证。

识别器耗时包含其预处理、推理和解析，不含输入裁切、gap-probe 分割、读盘、摘要和冷启动。
收紧文字行再等比放大会增加输入宽度和计算量；本轮不宣称提速或改善游戏帧率。
实际接入前仍需测采样延迟与游戏负载，并用独立会话验证拒绝率和符号误读。

## 本机证据与复现

证据根目录：`C:\Dcode\WWMAP-TOOLS\out\coordinate-line-study-20261015\`。

- `current/english-production-audit/`、`current/english-aspect-audit/`：四组原始三次核验与真实预处理 PNG。
- `current/english-clock-audit/`：对上述原生解码结果重放严格日期边界，未重新运行模型、未输入标签。
- `english-aspect-full/`：固定源码与参数后的全量复核。
- `analyze_clock_audits.py`：重放和评分入口。
- `finalize_study.py`、`study-verification.json`、`study-report.md`：摘要核对、608 条重放一致性和最终对照。
- `visual-overlap-observations.json`、`sign-hardcase-814.json`、`sign-hardcase-814-zoom.png`：后验复核与新增符号反例，非精确标签。
- `clock-counterfactual-test.log`、`clock-counterfactual-build.log`：反事实失败证据。
- `final-build-v2.log`、`final-tests-v2.log`：构建和 Python 测试；原生契约结果见 `native-*-tests.log`。

`--clock-prefix <native.json> <output.json>` 只重放原生报告的解码文本和分数，保留原模型来源，
并单列后处理器编译摘要；不会用原报告的 selected 坐标指导提取。已存在的输出会被拒绝。

```powershell
$dataRoot = 'C:\Dcode\WWMAP-TOOLS'
. (Join-Path $dataRoot 'scripts\Enter-DevEnvironment.ps1')
$python = Join-Path $dataRoot 'out\coord-ocr-compare\.venv\Scripts\python.exe'
.\scripts\Build-CoordinateOffline.ps1 -DependencyRoot $dataRoot
& $python tools\coordinate-ocr\benchmark.py `
    --data-root $dataRoot --upstream "$dataRoot\third_party\research\WutheringWaves-Navigator" `
    --native out\build\coordinate-offline\IMaoCoordinateOffline.exe `
    --ppocr-model "$dataRoot\out\coordinate-models\en_PP-OCRv5_mobile_rec_infer" `
    --corpus "$dataRoot\out\coordinate-minimap-check\full-corpus-v4.json" `
    --profiles readout-wide readout-line --preprocessing aspect --repeats 1 `
    --output "$dataRoot\out\coordinate-line-study-20261015\new-run"
```

没有将这批核验标签改为训练集，没有将近似小地图 XY 当成精确标签，也没有刷新或发布客户端。
