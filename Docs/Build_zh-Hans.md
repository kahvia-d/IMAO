# 可复现构建（Windows x64）

本项目由 WinUI 3（C#）前端和 `IMao-Core.dll`（C++）组成。构建入口均在
`scripts/` 中；不要依赖 CMake GUI 的手工配置。

## 版本基线

| 组件 | 固定版本 / 要求 |
| --- | --- |
| Visual Studio Build Tools | 2022，含 Desktop development with C++、CMake tools |
| Windows SDK | 10.0.26100.0 |
| .NET SDK | 8.x（运行时不能替代 SDK） |
| OpenCV | 4.11.0 + `opencv_contrib` 4.11.0，启用 `OPENCV_ENABLE_NONFREE=ON` |
| Paddle Inference | 3.0.0，Windows x64 / CPU / AVX / MKL |
| Git LFS | 用于 `Assets/FeaturesDatas/Map_features.yml` |

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

构建成功后，运行目录为 `x64/Release/`，其中包含 WinUI 应用、`IMao-Core.dll`、
`Assets/`、Paddle 运行库和 `opencv_world*.dll`。只验证 CMake 配置时，两个构建
脚本都支持 `-ConfigureOnly`。

## 脚本的职责

- `Build-OpenCV.ps1`：以 Ninja 构建包含 SURF 的最小 OpenCV 4.11.0 world DLL，自动应用离线 VGG 修补并跳过不需要的描述子下载。
- `Test-BuildPrerequisites.ps1`：检查编译器、SDK、LFS、assets 和三个外部依赖。
- `Build-IMao.ps1`：配置并编译 C++ 核心与 WinUI 前端，再收集运行时文件。

脚本会清理开发宿主中同时存在的 `Path` / `PATH` 环境变量冲突；这类冲突会使
MSBuild 失败。它还会把 .NET 首次运行状态与 NuGet 缓存隔离到被忽略的
`third_party/`。请优先调用脚本，而不是在普通终端中直接执行 CMake。

## 当前机器盘点（2026-08-23）

已检测到 VS 2022 Build Tools 17.14.3、MSVC 14.44、Windows SDK 10.0.26100.0、
VS 自带 CMake/Ninja 和 Git LFS 3.5.1；本工作区已准备好仓库内 .NET 8.0.424 SDK、
Paddle Inference 3.0.0 及 OpenCV 4.11.0 源码。

2026-08-23 已验证：预检通过；OpenCV world DLL 已按 nonfree/SURF 和 SIMD 配置构建；
`IMao-Core.dll` 与自包含 WinUI 应用均成功生成，运行目录中的 assets 校验一致。此验证
不启动游戏或图形界面，游戏版本兼容性仍应在下一阶段人工验证。
