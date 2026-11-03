# 坐标 YOLO 研究续接与清理保留清单

核对日期：2026-11-01。本页记录本机实际文件与脚本接口，供后续补采、补标、训练、导出和回放使用。用户当次判断是 YOLO 已相对稳定；本次工作是保留成果、素材与续接能力，不重新启动旧 Paddle 实验，也不把历史候选报告里的未升级状态当成当前产品需求。

## 当前成果及谱系

当前应用模型为 `Assets/models/coordinate-yolo/coordinate.onnx`。本次重新计算摘要，与 `model.json` 及 flight 导出记录一致：

| 文件（仓库相对路径） | SHA-256 |
|---|---|
| `Assets/models/coordinate-yolo/coordinate.onnx` | `f5abe76f9e9a982c502328a8e087525399c6a57074dd251a8835771686d561e4` |
| `out/coordinate-flight-intake-20261016/training/flight/weights/last.pt` | `2a917b5989e02c87bd6b913d1f91eb2eeb3427d0581a2c35e1691224bfdb3750` |
| `out/coordinate-targeted-retrain-20261016/training/expanded/weights/last.pt` | `62a7d8040a3159e4a8bd8744913e0f309176c55a5b7d9b2b4e7225f6e14f56df` |
| `out/coordinate-yolo-selftrain-20261015/official/yolo11n.pt` | `0ebbc80d4a7680d14987a577cd21342b65ecfd94632bd9a8da63ae6417644ee1` |

谱系为官方通用 YOLO11n → 本项目自有标注训练 → expanded 139 来源 → flight 164 来源。当前续训最直接的起点是 flight `last.pt`；保留 expanded 权重用于负对照及历史后备研究，保留官方初始化用于从头重建实验。不以 ONNX 代替可续训的 `.pt`；原训练固定取最后一轮，不能事后改成按分数挑 `best.pt`。

运行输入为 `[1,3,64,896]`，14 类顺序为 `0123456789-,+:`，conf=0.25、IoU=0.45、maxDet=256。应用使用同一权重的 raw/contrast 两路、数字优势确认、XY 解码及有证据的负号恢复；Z 可选。正式实现见 `IMao-Core/src/Coordinate/IdentifyWorldCoordinates/YoloCoordinateRecognizer.cpp` 与 `YoloCoordinateDecoder.cpp`。默认 CPU；当前源码还提供显式 DirectML 选择及失败回退 CPU。这里的运行后端回退与历史“双权重后备”是不同机制，当前模型目录不要求 Python 或 expanded 第二权重。

历史 flight 报告记录过退步、batch32 与单张的 2011/2012 差异；后续 recovery 报告记录 batch1 的 2012/2012。两份报告及原严格失败断言均保留，不能把旧失败改成通过，也不能把已经观察过的会话重新称为独立验收。

## 保留位置与原始素材

本次采用**原路径保留**，不把仍有绝对路径引用的研究树整体搬到 archive。`out` 中这些文件多数被 git 忽略，普通 clone 不含它们；备份必须显式包含。

| 保留范围 | 用途 |
|---|---|
| `Assets/models/coordinate-yolo/` 整目录（含 DirectML 子目录、DLL、许可证、model.json） | 当前可运行成果 |
| 上表三个 `.pt`，flight/export/flight.onnx、targeted/export/expanded.onnx | 续训、谱系、历史对照 |
| `tools/coordinate-ocr/`，尤其整个 annotations | 真值版本、原始框、冻结源码、完整压缩检测结果 |
| `out/coordinate-samples/session-1F638949-A4DA-46C7-8131-3F08EEFD62FD/` | 首批真实截图、采集索引；100 个训练来源在此 |
| `out/coordinate-independent-20261016/capture/` | 110DF22E、B28F69A4、448C5F2E、94B68642 原始会话及同帧小地图；含负例、困难帧和后续研究素材 |
| `out/coordinate-independent-20261016/full-110DF22E/inputs/readout-line/` | 39 个新增训练来源所在裁切目录 |
| `out/coordinate-flight-intake-20261016/inputs/`、`native/` | 飞行会话435帧原裁切及原生 raw/contrast；25个新训练来源在 inputs/readout-line |
| flight 的 `flight/`、`control/` 和 targeted 的 `expanded/` | 实际 YOLO images、labels、train.txt、data.yaml |
| 各研究目录的小型 `.py/.ps1/.json/.jsonl/.yaml/.txt/.csv/.log` 等 | 自定义脚本、标注、冻结锁、参数、训练曲线、失败与成功证据 |
| selftrain 的 `.gpu-venv/`、`settings/`、`pilot-v2/pilot-manifest.json` | 现有训练入口与辅助字符框模板 |

实际 operative 真值为 flight `labels-v3-user-confirmed.json`；训练框与实际参数以 `training-protocol-v2.json` 为准。v1 是准备稿，v2 修正198/239/292辅助时钟；历史版本保留供审计，不能覆盖。164行 `sourceImage` 的100+39+25个引用本次检查全部存在，没有一个指向 Paddle finetune 目录。框标注不仅包含 XY，也包含可见 Z、日期和时钟，不能把这些可见字符当背景。

飞行会话1–338是原训练候选段，339–363为缓冲，364–435是历史留出；46张无坐标、3张待定（71/160/237）保留，待定不强填真值。原25张训练来源为12、16、22、70、74、79、150、155、159、194、198、201、236、239、242、254、256、261、285、289、292、310、325、334、336。重要反例包括飞行21数字误认、88批量边界、旧会话415耀光、506/593/596拒识及恢复报告所列损失帧。它们连同同帧索引保留，比只留漂亮成功截图更有用。

旧1577个有效输入的图片引用在 targeted `evaluation-inputs.json` 中；文件还包含一个被明确排除的835。raw/contrast引用共3156个，本次均存在。须保留这些文件的精确集合，目录为：

- `out/coordinate-gap-study-20261015/native-preprocessed/`（692个引用）；
- `out/coordinate-yolo-expansion-20261015/dataset/images/`（40）；
- `out/coordinate-yolo-expansion-20261015/native-preprocessed/`（40）；
- `out/coordinate-sign-recovery-20261016/native-training/`（48）；
- `out/coordinate-sign-layout-20261015/native-preprocessed/`（12）；
- `out/coordinate-independent-20261016/full-110DF22E/native/`（2324）。

这些是有标签回归的必要预处理图，不因目录名带 gap/旧实验就批量删除。只保留原图而删除这批图，会使现有回放入口缺文件。

## 环境：现有指针环境并不便携

训练时记录 Python3.12.7、Ultralytics8.3.228、torch2.9.1+cu130、torchvision0.24.1+cu130、RTX4060 8GB。`.gpu-venv/pyvenv.cfg` 使用系统包，`cached-gpu-runtime.pth` 又引用工作区之外的8个 uv 缓存目录。本次逐项确认这些缓存存在，安装元数据为：

| 包 | 已安装元数据版本 |
|---|---|
| torch / torchvision | 2.9.1+cu130 / 0.24.1+cu130 |
| ultralytics / ultralytics-thop | 8.3.228 / 2.0.18 |
| onnx / ml_dtypes | 1.19.0 / 0.5.4 |
| polars / polars-runtime-32 | 1.35.2 / 1.35.2 |

当前基环境的元数据为 numpy1.26.4、opencv-python5.0.0.93、onnxruntime1.31.0、Pillow10.4.0、PyYAML6.0.1、scipy1.17.1、matplotlib3.9.2、pandas2.2.2、psutil5.9.0、requests2.32.3。这是**本次盘点时**的基环境，不冒充2026-10-16完整锁文件。普通 `python` 当前是 torch2.11.0，且缺少 ultralytics/torchvision/onnx；不能直接替代上述训练环境。

保留小型指针环境后，本机仍依赖工作区外 Python 和 uv 缓存。跨机续接前，应从本机现有安装/缓存制作完整依赖归档并记录版本、哈希及许可证；只复制 `.gpu-venv` 不够。2026-11-01 清理前实际导入 torch、torchvision、ultralytics、onnx、cv2 成功，版本与上表一致，CUDA 可用；没有重新训练或下载。这不是干净机器的安装验证。若需要超过约100MB的新下载，先征询用户。

## 实际训练、导出与回放入口

所有 build/test/publish 前先 `. .\scripts\Enter-DevEnvironment.ps1`。以下是续接操作说明，不表示本次已经运行训练或测试。

```powershell
Set-Location C:\Dcode\WWMAP-TOOLS
. .\scripts\Enter-DevEnvironment.ps1
$yoloPython = '.\out\coordinate-yolo-selftrain-20261015\.gpu-venv\Scripts\python.exe'
& $yoloPython -c 'import torch, torchvision, ultralytics, onnx, cv2; print(torch.__version__, torchvision.__version__, ultralytics.__version__, onnx.__version__, cv2.__version__); print(torch.cuda.is_available())'
```

历史实际数据集生成顺序为 flight `prepare_training.py` → 原图框/时钟QA → `finalize_training.py` → `train.py`。后者验证v2清单、每张图和每份label摘要，实际各训control/flight40轮：AdamW、lr0=0.0001、seed=2026101604、batch8、imgsz896、rect=true、device0、AMP关闭、增强关闭。`val=train`只看拟合，不是泛化成绩。

这些历史脚本**没有通用参数接口**，且用 `assert not exists()` 防止覆盖历史产物；不能在已保留目录里原样重跑。重建/新实验应复制脚本到新研究目录，显式修改根路径、输出名和新协议，保留旧脚本及旧断言。准备脚本还读取 targeted expanded 原图和 pilot-v2模板，不可只拷贝一个 train.py。

使用现成保留数据继续训练，可复用原 `flight-training.json` 的完整 args（避免手抄漏参数），仅明确更改训练起点、输出目录和新实验名。下面是直接调用项目已经使用的 Ultralytics Python API；先完成环境自检，再执行：

```powershell
@'
from pathlib import Path
import json, os
root = Path.cwd()
os.environ['YOLO_AUTOINSTALL'] = 'false'
os.environ['YOLO_CONFIG_DIR'] = str(root/'out/coordinate-yolo-selftrain-20261015/settings')
os.environ['CUBLAS_WORKSPACE_CONFIG'] = ':4096:8'
from ultralytics import YOLO
args = json.loads((root/'out/coordinate-flight-intake-20261016/flight-training.json').read_text(encoding='utf-8-sig'))['args']
args.update(project=str(root/'out/coordinate-yolo-resume'), name='flight-continuation-v1')
assert not (Path(args['project'])/args['name']).exists()
YOLO(str(root/'out/coordinate-flight-intake-20261016/training/flight/weights/last.pt')).train(**args)
'@ | & $yoloPython -
```

这会开始**新的续训**，不是逐位重现旧flight训练，也不自动证明有收益。原样重现旧40轮须从 expanded 权重开始并保留原v2清单与环境。新增截图时，以新的来源清单和人工QA生成新的 images/labels/train.txt/data.yaml；YOLO标签每行为 `class centerX/892 centerY/48 width/892 height/48`，每来源有raw/contrast两个48×892变体，14类顺序不变。`prepare_training.py` 的归一化和完整上下文框是实际实现参照。

导出仍使用实际历史接口，输入是新训练的last.pt：

```powershell
@'
from pathlib import Path
import os
os.environ['YOLO_AUTOINSTALL'] = 'false'
from ultralytics import YOLO
import onnx
weights = Path('out/coordinate-yolo-resume/flight-continuation-v1/weights/last.pt')
assert weights.is_file() and not weights.with_suffix('.onnx').exists()
exported = YOLO(str(weights)).export(format='onnx', imgsz=(64,896), opset=12, dynamic=False, simplify=False, half=False, batch=1, device='cpu')
onnx.checker.check_model(onnx.load(exported))
print(exported)
'@ | & $yoloPython -
```

历史实际评估入口为 flight `evaluate_training.py`（GPU batch32）、`export_and_cpu_check.py`（CPU单张，旧严格一致断言预期记录失败），以及 recovery `runtime_replay.py gpu|cpu`（batch1）。后者依赖：flight `evaluate_training.load_inputs`、independent目录冻结的 `xy_decode.py/sign_recovery.py/sign_layout.py/benchmark.py` 和 `calibration.json`、uncertainty目录 `probe.py`、recovery `agreement_probe.py`、targeted的输入/检测/评分JSON、flight的v2训练协议与v3真值。原入口全是固定历史权重、固定输出名，不能拿新模型替换旧文件来凑运行；为新模型复制入口并设置新的输出及锁，单模型比对 `primarySelected`，双权重后备只作为单独实验。

正式原生离线工具真实CLI为：

如果旧编译输出已随构建产物清理，先按 [构建指南](Build_zh-Hans.md) 准备工具链并重新构建 `IMao-Core/tools/CoordinateYolo` 对应目标；本次保留源码与依赖，不承诺旧工具 EXE 一直存在。

```text
IMaoCoordinateYolo.exe decode|infer|preprocess model-directory manifest.json report.json
```

manifest是数组，每行含id、images.raw、images.contrast、observations.raw/contrast、calibrated；preprocess另需crop。`infer`重新检测，`decode`使用冻结框，`preprocess`逐像素比较raw/contrast。源码在 `IMao-Core/tools/CoordinateYolo/main.cpp`；输入均为路径引用，不能把历史整个报告对象直接当manifest。当前原生Prepare可在不加载Paddle的情况下校验预处理，新增生成器必须按其实现产图并与保留冻结图做逐像素核对。

移植对比脚本真实参数为：

```text
python tools/coordinate-ocr/verify_native_yolo.py --reference <runtime-cpu-v1.json> --decoder <decoder.json> --inference <inference.json> --preprocessing <preprocessing.json> --output <新的结果.json>
```

它比较reference的`primarySelected`与原生`selected`，并检查预处理samePixels；这是移植一致性，不是新会话准确率。新模型仍需单张CPU实际推理、正确/错误/拒识/无坐标误报、性能及新会话验收，不能只看训练mAP或旧会话。

## 可清理的历史物与复现边界

Paddle训练权重、优化器状态、导出模型、下载包、PaddleOCR源码副本及其一次性环境属于被替代实验，可在自定义脚本/配置/报告快照和YOLO依赖清单核对后清理。包括 `out/coordinate-finetune-20261015/` 中的大型`.pdparams/.pdopt/.pdiparams`、downloads、`.venv/.export-venv`、PaddleOCR-source。重复图册、生成缓存、未采用中间训练轮次和无标签的派生放大/淡化图也可列候选，不能据扩展名删除当前YOLO `.pt`、原始截图或上述回归图。

删除后以下历史路径明确属于**保留记录、缺少可执行依赖**：flight `run_frozen.py` 通过旧 `IMaoCoordinateOffline.exe` 和 `coordinate-finetune-20261015/coverage-v2/coverage-positive-infer-paddle30` 获得预处理图。这条旧Paddle路径不再承诺可重跑；当前续训使用已保留原生图和YOLO数据集，新增预处理走当前原生Prepare。不能为了让旧入口可跑而恢复整套过时训练资产。旧pilot、mixed、snow及control/refined等未采用权重若按清单移除，直接引用它们的历史评估/pack脚本也只保留方法与结果，不能再声称可原样全流程重放；本页续训保留的模型范围是官方初始化、expanded最后一轮、当前flight最后一轮及两者ONNX。

另有外部路径耦合：recovery `verify.py`、independent `onnx_reference.py`、部分pack/snapshot脚本硬编码旧 `coordinate-offline-evaluation` worktree。本次确认该worktree存在，但仓库备份不能假定它一直存在。恢复时从本仓库对应annotations读取证据，在**新副本**中改根路径；不要改动原冻结脚本及其摘要断言。`train.txt`、data.yaml、清单、锁和评分JSON也含绝对根路径，换机器需派生新的可迁移清单并记录映射，不能声称旧锁仍原样成立。

逐文件保留清单与 SHA-256 位于 `out/evidence/research-retention-20261101/yolo-retained-files.json`，执行记录见 [研究成果保留入口](ResearchArtifactRetention_zh-Hans.md)。实际清理结果以删除记录及删除后依赖核对为准。未进行训练、下载、构建或客户端刷新，也未改任何既有真值或断言。
