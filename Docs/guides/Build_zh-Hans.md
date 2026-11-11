# 可复现构建（Windows x64）

本项目由 WinUI 3（C#）前端和独立的 `IMao-CoreHost.exe`（C++）组成，通过命名管道通信。构建入口均在
`scripts/` 中；不要依赖 CMake GUI 的手工配置。

## 使用范围与本次核对

本文命令均从仓库根目录执行，不是在 `Docs/guides/` 内执行。2026-11-12 文档核对确认：`global.json` 仍固定 .NET SDK 8.0.424；历史机器盘点所述的两个缺失工具源码目前均已纳入 Git。本文下方的构建与测试结果保留原验证日期，本次文档整理未重新构建程序。

任何 build / test / publish 前先加载 `scripts/Enter-DevEnvironment.ps1`。托管或 XAML 改动须跑真实 WinUI 构建；交付须 self-contained，XAML 补丁须 publish 并整树同步含 `resources.pri`，不得用托管测试项目代替 WinUI 门禁。发布流程见[云端构建、本地签名发布](../CloudRelease.md)。

## 版本基线

| 组件 | 固定版本 / 要求 |
| --- | --- |
| Visual Studio Build Tools | 2022，含 Desktop development with C++、CMake tools |
| Windows SDK | 10.0.26100.0 |
| PowerShell | 7 或更高，`pwsh.exe` 在 PATH 中 |
| .NET SDK | 8.0.424，按 `global.json` 固定（运行时不能替代 SDK） |
| SDL | 3.4.18，锁定官方源码、输入补丁及 MSVC 可复现构建配方 |
| OpenCV | 4.11.0 + `opencv_contrib` 4.11.0，启用 `OPENCV_ENABLE_NONFREE=ON` |
| Paddle Inference | 3.0.0，Windows x64 / CPU / AVX / MKL；现有 CMake/兼容工具仍依赖，不代表应用坐标识别仍使用 Paddle |
| 坐标 YOLO | `Assets/models/coordinate-yolo/` 的 `model.json`、ONNX 与配套 ONNX Runtime；CPU / DirectML 共用模型 |
| Git LFS | 用于当前区域包特征、索引、模型等大文件；以 `.gitattributes` 为准，旧基础 `Map_features.yml` 已退役 |

发行包中的 `opencv_world4110.dll` 确定了 OpenCV 4.11.0 基线。本项目使用
`cv::xfeatures2d::SURF`，因此普通 OpenCV 预编译包不够：必须包含
`opencv_contrib` 并启用 nonfree。

## 依赖目录

依赖不进入 Git；默认目录均位于被忽略的 `third_party/` 下：

```text
third_party/
  paddle-inference-3.0.0/       # Paddle 解压根目录（含 paddle/ 与 third_party/）
  src/opencv-4.11.0/
  src/opencv_contrib-4.11.0/
  build/opencv-4.11.0/          # OpenCV 的 CMake 输出，含 OpenCVConfig.cmake
tools/dotnet-sdk-8.0.424/dotnet.exe  # 可选：仓库内的 .NET 8 SDK
```

只有确需补齐依赖时才获取源码，优先复用本机已有等价物；超过约 100 MB 的下载须先取得用户同意。OpenCV 固定使用标签 `4.11.0`：

```powershell
git clone --depth 1 --branch 4.11.0 https://github.com/opencv/opencv.git third_party/src/opencv-4.11.0
git clone --depth 1 --branch 4.11.0 https://github.com/opencv/opencv_contrib.git third_party/src/opencv_contrib-4.11.0
```

Paddle 使用官方 3.0.0 的 Windows CPU/AVX/MKL C++ 包；解压后将其根目录设为
`IMAO_PADDLE_LIB`。若系统未安装 .NET 8 SDK，可将 SDK 解压到
`tools/dotnet-sdk-8.0.424/`。

## 首次构建

依赖按上述默认目录准备后，可在每个新的 PowerShell 会话中加载仓库环境：

```powershell
. .\scripts\Enter-DevEnvironment.ps1
.\scripts\Test-BuildPrerequisites.ps1
.\scripts\Build-IMao.ps1
```

环境加载脚本仅设置当前进程的变量，不修改系统 PATH；不会自动下载依赖。

后续构建默认复用中间文件；检测到 MSVC 工具集或 Windows SDK 变化时才清理。需要主动清理配置与目标文件时使用 `Build-IMao.ps1 -FreshConfigure`。运行回归可使用 `Test-Runtime.ps1 -OutputDirectory .\out\runtime-tests`；该脚本默认 4 个编译任务，内存较少时加 `-Parallel 2`。

```powershell
git lfs install
# git lfs pull 可能下载大量资源；先核对本机已有文件与预计下载量，超过约 100 MB 先征求同意。
git lfs pull

# 仅首次或 OpenCV 版本变更后执行；脚本使用 VS 自带 Ninja 与 MSVC。
pwsh -NoProfile -ExecutionPolicy Bypass -File .\scripts\Build-OpenCV.ps1

# 复用 Enter-DevEnvironment 已解析的依赖路径；不要再覆盖它对嵌套 Paddle 目录的识别。

pwsh -NoProfile -ExecutionPolicy Bypass -File .\scripts\Test-BuildPrerequisites.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File .\scripts\Build-IMao.ps1
```

## 当前资源与输出布局

当前定位资源来自区域包，不需要为了编译而重建旧基础图集。`Map_features.yml` 已退役；`Build-FeatureBinary.ps1` 是恢复旧 XML 后重建基础包的历史工具，缺少该源文件时会明确拒绝，不能作为普通首次构建步骤。

`Test-BuildPrerequisites.ps1` 与 `Build-IMao.ps1` 当前核对：

- 基础 `Map_features.imf` / `Map_visual_index.imx` 要么同时存在，要么同时缺席；当前区域包布局允许二者都不存在。
- `kuro-tile-packs.json` 中登记的包必须有 manifest、`features.imf` 和 `visual-index.imx`，不再允许早期四包指南中的“先登记、以后再安装”。
- 不自带词表的索引分片需要共用 `Map_visual_vocabulary.imx`；构建和运行时核对依赖，不能随意重新聚类词表后混用旧分片。
- `Assets/models/coordinate-yolo/model.json` 列出的模型与运行库必须存在，大小和 SHA-256 一致；检查不通过会阻止构建。
- Release 不得暂存旧基础 `Map_features.yml`，资源由 `cmake/StageAssets.cmake` 收集。

区域重建入口是 `scripts/Invoke-MapRegionRebuild.ps1`，默认只验证计划，显式 `-Apply` 才生成；参考图、四点校准与输出目录要求见[瓦片特征包指南](KuroTileFeaturePacks.md)。常规构建使用已准备的资源，不应为了通过预检自行重建或替换地图。

构建输出在 `x64/Release/`，包含 WinUI、独立 `IMao-CoreHost.exe`、Assets 和所需运行库。`Build-IMao.ps1` 在完成后执行 CoreHost 的 `--check-resources`，并验证 WinUI 自包含运行时；这不等于游戏实机验收。`Build-IMao.ps1` 与 `Build-OpenCV.ps1` 的 `-ConfigureOnly` 仅配置 CMake。

## 脚本的职责

- `Build-OpenCV.ps1`：以 Ninja 构建包含 SURF 的最小 OpenCV 4.11.0 world DLL，自动应用离线 VGG 修补并跳过不需要的描述子下载。
- `Test-BuildPrerequisites.ps1`：检查编译器、SDK、LFS、assets 和三个外部依赖。
- `Build-IMao.ps1`：配置并编译 C++ 核心与 WinUI 前端，再收集运行时文件。
- `Build-FeatureBinary.ps1`：历史基础包转换入口，必须先恢复已退役的 XML 源；不是当前区域包的重建入口。
- `Build-VisualIndex.ps1`：基础 IMF 存在时重建索引，默认复用 `Map_visual_vocabulary.imx`，不创建一套不兼容的新词表。
- `Invoke-MapRegionRebuild.ps1`：规划或生成区域包；有无参考图的产物验证资格不同。
- `New-CoordinateRegressionManifest.ps1`：从已有诊断会话生成坐标回归清单。
- `New-VisualLocalizationManifest.ps1`：登记现有小地图裁剪并生成旋转、亮度、模糊、比例、遮挡与拒绝样本。
- `Test-Performance.ps1`：运行 IMF 多进程加载基准并汇总诊断日志为 JSON。

## 回归入口与适用范围

```powershell
. .\scripts\Enter-DevEnvironment.ps1
.\scripts\Test-Runtime.ps1 -OutputDirectory .\out\runtime-tests
```

上述脚本是工程回归入口，托管/XAML 变更仍需真实 WinUI 构建。当前坐标模型为自训 YOLO；研究素材、训练谱系、原生回放和边界见[坐标 YOLO 续接](CoordinateYoloResume_zh-Hans.md)及[裁剪鲁棒性记录](../research/CoordinateYoloCropRobustness_zh-Hans.md)。

`IMaoCoordinateRegression` 与 `New-CoordinateRegressionManifest.ps1` 仍保留旧 PP-OCR 回归链，前者源码明确加载 `PP-OCRv5_mobile_rec_infer`。它们可用于具备历史模型的对照实验，不能证明当前 YOLO 的识别效果；当前资源缺少该旧模型时不能直接照旧命令运行。`CoordinateOfflineEvaluation_zh-Hans.md` 中的 `production` profile 也是 2026-10-14 冻结的 Paddle 取景名称。

当前 YOLO 取景由 `YoloCoordinateRecognizer::Region` 定义，1600×900 参考框为 `(20,878)-(400,899)`，按 HUD 布局映射；小于 60×16 或越界会拒绝。不能再套用旧 PP-OCR 的 24 像素高度门槛，也不能承诺任意比例都可用。

视觉索引校验失败必须报告；当前 OCR 与视觉定位有独立读数和交接路径，不能沿用旧文档“任何视觉失效时 OCR 永远不能发布位置”的概括。具体资格、时效与显示限制见[视觉定位说明](../architecture/VisualLocalization_zh-Hans.md)。

脚本会清理开发宿主中同时存在的 `Path` / `PATH` 环境变量冲突；这类冲突会使
MSBuild 失败。它还会把 .NET 首次运行状态与 NuGet 缓存隔离到被忽略的
`third_party/`。请优先调用脚本，而不是在普通终端中直接执行 CMake。

## 历史验证（2026-09-07）

`C:\Dcode\WWMAP-TOOLS` 的 Release CoreHost 与 WinUI 已构建成功，资源就绪检查三项全部通过。
原生优化测试、90 项托管运行测试、大小地图回放及后台任务取消回归均通过。
此前缺失的两个工具源码现在已存在，旧记录中的 CMake 生成阻塞已不再发生。
正式入口默认增量构建，本次验证未重编译未变化的核心目标文件。
测试范围、性能对比和仍需游戏内验证的项目见 [核心重构记录](../research/Refactor_20260907.md)。

## 历史机器盘点

### 2026-09-05 新工作区验证

在 `C:\Dcode\WWMAP-TOOLS` 已配置 .NET SDK 8.0.424、Windows SDK
10.0.26100.0、Paddle Inference 3.0.0 和 OpenCV 4.11.0（nonfree/SURF）。
环境预检、Git LFS 指针检查、OpenCV 构建和 WinUI Release 编译通过；
WinUI 编译有 26 个现有警告、0 个错误。

完整原生构建仍被当前 Git 检出缺少的两个源文件阻塞：
`tools/KuroMapFeatureBuilder/main.cpp` 和
`IMao-Core/tools/VisualIndexBuilder/main.cpp`。CMake 在生成阶段失败，
因此尚未验证完整运行目录。两条路径均匹配 `.gitignore` 中的 `*build*/`
规则；需要从原开发工作区恢复真实源码并纳入版本控制。

以下为旧工作区的历史记录：

已检测到 VS 2022 Build Tools 17.14.3、MSVC 14.44、Windows SDK 10.0.26100.0、
VS 自带 CMake/Ninja 和 Git LFS 3.5.1；本工作区已准备好仓库内 .NET 8.0.424 SDK、
Paddle Inference 3.0.0 及 OpenCV 4.11.0 源码。

2026-08-23 已验证：预检通过；OpenCV world DLL 已按 nonfree/SURF 和 SIMD 配置构建；
`IMao-Core.dll` 与自包含 WinUI 应用均成功生成，运行目录中的 assets 校验一致。此验证
不启动游戏或图形界面，游戏版本兼容性仍应在下一阶段人工验证。
