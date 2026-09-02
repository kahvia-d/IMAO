# 小地图视觉定位

运行时以基础 `Map_visual_index.imx` 和可选特征包内的 `visual-index.imx` 分片做全局视觉
定位，OCR 只提供候选排序提示。任何位置提交都必须通过相似变换 RANSAC；任一必需 IMX
不可用、结果歧义或证据不足时，系统保持上一可信位置最多 2 秒，之后隐藏小地图位置相关
标记。

## 资源生成

```powershell
.\scripts\Build-FeatureBinary.ps1
.\scripts\Build-VisualIndex.ps1
```

索引生成器使用固定随机种子从基础 IMF 训练 4096 词词表，基础 IMF 生成主索引；当前
Kuro 特征包和候选特征分别生成引用相同词表的分片。运行时只合并倒排表、分块和只读
特征行，不重新训练词表。输出前会重新加载临时 IMX，逐字段核对词表、分块、特征行和
倒排表；验证成功后才替换正式资源与清单。

## 离线回归

```powershell
.\scripts\New-VisualLocalizationManifest.ps1
cmake --build --preset windows-x64-release-visual-regression
.\x64\Release\IMaoVisualRegression.exe $PWD .\Tests\VisualLocalization\manifest.json .\x64\Performance\visual-regression.json
.\scripts\Test-Performance.ps1
```

`visual-regression.json` 包含前 12 个候选、场景、地图中心、旋转、比例、内点数、内点率、
误差、象限覆盖、粗检索和几何验证耗时。命令在任一带真值或 `mustReject` 样本发生错误
接受时返回非零退出码。

## 诊断模式

诊断构建默认 `visual`。需要对照时，在启动前设置：

```powershell
$env:IMAO_LOCALIZATION_MODE = 'compare' # visual | legacy | compare
```

`legacy`/`compare` 会运行旧 OCR 附近验证并写入 `legacy-localization-result`，但视觉链路仍
是唯一发布者。日志还记录粗检索、几何验证、局部跟踪、OCR 加权、歧义拒绝和恢复耗时，
并以 2 秒节流保存接受/拒绝的小地图裁剪，不重复保存完整帧。
