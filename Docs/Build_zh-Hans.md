# 可复现构建（Windows x64）

本项目由 WinUI 3（C#）前端和独立的 `IMao-CoreHost.exe`（C++）组成，通过命名管道通信。构建入口均在
`scripts/` 中；不要依赖 CMake GUI 的手工配置。

## 版本基线

| 组件 | 固定版本 / 要求 |
| --- | --- |
| Visual Studio Build Tools | 2022，含 Desktop development with C++、CMake tools |
| Windows SDK | 10.0.26100.0 |
| .NET SDK | 8.x 或更高（运行时不能替代 SDK） |
| OpenCV | 4.11.0 + `opencv_contrib` 4.11.0，启用 `OPENCV_ENABLE_NONFREE=ON` |
| Paddle Inference | 3.0.0，Windows x64 / CPU / AVX / MKL |
| Git LFS | 用于 `Map_features.yml`、`Map_features.imf`、基础/可选 `*.imx` 与生成清单 |

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

需要准备 OpenCV 源码时，固定使用标签 `4.11.0`：

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
git lfs pull

# 仅首次或 OpenCV 版本变更后执行；脚本使用 VS 自带 Ninja 与 MSVC。
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\Build-OpenCV.ps1

$env:IMAO_PADDLE_LIB = "$PWD\third_party\paddle-inference-3.0.0"
$env:IMAO_OPENCV_DIR = "$PWD\third_party\build\opencv-4.11.0"
$env:IMAO_DOTNET = "$PWD\tools\dotnet-sdk-8.0.424\dotnet.exe"  # 系统已安装 .NET 8 SDK 时可省略

powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\Test-BuildPrerequisites.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\Build-IMao.ps1
```

若仓库只有 XML 源而缺少 `Map_features.imf`，先执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\Build-FeatureBinary.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\Build-VisualIndex.ps1
```

转换器显式写入小端字段、SHA-256 和固定 128 列 Float32 描述子；它会重新加载临时
IMF，与 XML 中全部关键点逐字段、描述子逐字节比较，成功后才原子替换 IMF 与清单。
视觉索引脚本随后以固定随机种子生成 4096 个视觉词、384×384/步长 192 的重叠分块，
并在原子替换前完整回读、验证 IMF 哈希、负载哈希、行号和倒排表。

构建成功后，运行目录为 `x64/Release/`，其中包含 WinUI 应用、`IMao-Core.dll`、
`Assets/`、Paddle 运行库和 `opencv_world*.dll`。Release 只暂存基础地图 IMF，不
暂存 `Map_features.yml`；诊断构建仍允许 XML 回退。只验证 CMake 配置时，两个构建
脚本都支持 `-ConfigureOnly`。

## 脚本的职责

- `Build-OpenCV.ps1`：以 Ninja 构建包含 SURF 的最小 OpenCV 4.11.0 world DLL，自动应用离线 VGG 修补并跳过不需要的描述子下载。
- `Test-BuildPrerequisites.ps1`：检查编译器、SDK、LFS、assets 和三个外部依赖。
- `Build-IMao.ps1`：配置并编译 C++ 核心与 WinUI 前端，再收集运行时文件。
- `Build-FeatureBinary.ps1`：编译转换器并生成、回读验证 IMF 与清单。
- `Build-VisualIndex.ps1`：编译索引器并生成、回读验证 IMX 与清单。
- `New-CoordinateRegressionManifest.ps1`：从已有诊断会话生成坐标回归清单。
- `New-VisualLocalizationManifest.ps1`：登记现有小地图裁剪并生成旋转、亮度、模糊、比例、遮挡与拒绝样本。
- `Test-Performance.ps1`：运行 IMF 多进程加载基准并汇总诊断日志为 JSON。

## 优化回归

```powershell
cmake --build --preset windows-x64-release-optimization-tests
.\x64\Release\IMaoOptimizationTests.exe
.\scripts\New-CoordinateRegressionManifest.ps1
cmake --build --preset windows-x64-release-coordinate-regression
.\x64\Release\IMaoCoordinateRegression.exe $PWD .\Tests\CoordinateRegression\manifest.json .\x64\Performance\coordinate-regression.json
cmake --build --preset windows-x64-release-visual-regression
.\scripts\New-VisualLocalizationManifest.ps1
.\x64\Release\IMaoVisualRegression.exe $PWD .\Tests\VisualLocalization\manifest.json .\x64\Performance\visual-regression.json
.\scripts\Test-Performance.ps1 -FeatureLoadRuns 5
```

坐标回归会分别执行 CLAHE 与顶帽单路请求，并将联合候选的地图平面命中率、三维文本
完全命中率和单路推理 P50/P95 写入 `coordinate-regression.json`；综合性能报告默认写入
`x64/Performance/optimization-report.json`。若启动提示 IMF 魔数、
版本、长度、描述子形状或 SHA-256 错误，不要放宽校验；重新拉取 Git LFS 资源或运行
`Build-FeatureBinary.ps1`。非 1600×900、1920×1080、2560×1440 的画面会安全拒绝
坐标 OCR，并在诊断日志中记录 `ocr-unsupported`。

Release 必须同时含 `Map_features.imf`、基础 `Map_visual_index.imx` 与已有可选特征包的
`visual-index.imx` 分片，且不得含基础 `Map_features.yml`。IMX 缺失、IMF/词表哈希不一致、
倒排表损坏或视觉结果歧义时，运行时不会退回 OCR 独立提交位置。诊断构建可设置
`IMAO_LOCALIZATION_MODE=visual|legacy|compare`；`legacy` 与 `compare` 中的旧链路只写
对照日志，始终不能发布位置。

脚本会清理开发宿主中同时存在的 `Path` / `PATH` 环境变量冲突；这类冲突会使
MSBuild 失败。它还会把 .NET 首次运行状态与 NuGet 缓存隔离到被忽略的
`third_party/`。请优先调用脚本，而不是在普通终端中直接执行 CMake。

## 当前验证（2026-09-07）

`C:\Dcode\WWMAP-TOOLS` 的 Release CoreHost 与 WinUI 已构建成功，资源就绪检查三项全部通过。
原生优化测试、90 项托管运行测试、大小地图回放及后台任务取消回归均通过。
此前缺失的两个工具源码现在已存在，旧记录中的 CMake 生成阻塞已不再发生。
正式入口默认增量构建，本次验证未重编译未变化的核心目标文件。
测试范围、性能对比和仍需游戏内验证的项目见 [核心重构记录](Refactor_20260907.md)。

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
