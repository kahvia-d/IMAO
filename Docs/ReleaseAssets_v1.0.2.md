# v1.0.2 发行资源归档

归档来源：上游 `IMao-Wuthering-Waves` v1.0.2 Windows x64 发行包，目录为
`IMao/Assets`。这些文件于 2026-08-23 从本地解压包逐文件 SHA-256 校验后
复制到仓库根目录的 `Assets/`，以匹配 C++ 核心在运行时使用的相对路径。

## 已归档内容

| 目录 | 用途 | 文件数 | 大小 |
| --- | --- | ---: | ---: |
| `Assets/FeaturesDatas` | 游戏状态/地图 SURF 特征数据库 | 3 | 459.99 MiB |
| `Assets/models` | PaddleOCR 检测、识别和方向分类模型 | 11 | 22.04 MiB |
| `Assets/Fonts` | ImGui 覆盖层字体 | 1 | 18.79 MiB |
| `Assets/th.jpg`、`Assets/WindowIcon.ico` | 发行版界面资源 | 2 | 3.59 MiB |
| **合计** | 运行所需资源 | **16** | **505.29 MiB** |

完整文件清单与 SHA-256 在
[`ReleaseAssets_v1.0.2.sha256`](ReleaseAssets_v1.0.2.sha256) 中。可在
PowerShell 中运行下列命令验证当前归档：

```powershell
Get-Content Docs/ReleaseAssets_v1.0.2.sha256 | ForEach-Object {
  if ($_ -match '^([0-9A-F]{64}) \*(.+)$') {
    $actual = (Get-FileHash -Algorithm SHA256 $matches[2]).Hash
    if ($actual -ne $matches[1]) { throw "Checksum mismatch: $($matches[2])" }
  }
}
```

`Assets/FeaturesDatas/Map_features.yml` 为 459.71 MiB，超过 GitHub 普通 Git
单文件上限，已由仓库的 `.gitattributes` 标记为 Git LFS 文件。提交前请执行
`git lfs install`；克隆者需使用常规 LFS 克隆或运行 `git lfs pull`。

## 同时保存的示例数据

发行包中的 8 个路线 JSON 已放入 `Examples/SavedRoutes/`（共 90 KiB）。它们是
只读示例，不会在应用启动时自动写入用户运行目录；需要时可手工复制到运行目录
中的 `SavedRoutes/`。

## 未纳入源码的发行版文件

整包约 970 MiB，其中大部分是 Windows App SDK 自包含运行时、.NET 发布产物和
可由构建恢复的依赖，不应作为源码归档。尤其未复制以下原生 DLL：

- `opencv_world4110.dll`
- `paddle_inference.dll`
- `mklml.dll`、`mkldnn.dll`、`libiomp5md.dll`、`common.dll`
- `IMao-Core.dll`、`IMao-WinUI.exe` 及 WinUI/.NET 发布文件

构建仍应按 `Docs/Compile_zh-Hans.md` 准备 OpenCV（含 SURF）和 Paddle C++ 推理
库。若需要复现发行包，先从这些构建依赖生成原生 DLL，再将根目录 `Assets/` 放到
最终可执行文件同级目录。

## 许可与溯源提醒

本归档仅记录“随上游 v1.0.2 发行包取得”的来源，未独立确认其中 Microsoft YaHei
字体、PaddleOCR 模型及地图特征数据的再分发许可。公开发布新版本前，应逐项确认
这些资源和第三方二进制的许可证及署名要求。
