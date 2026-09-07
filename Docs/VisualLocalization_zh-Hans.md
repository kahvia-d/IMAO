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

发布目录可在不启动游戏的情况下使用实际运行时加载器自检：

```powershell
.\x64\Release\IMao-CoreHost.exe --check-resources .\x64\Release\Assets
```

成功时 `resourcesReady`、`visualReady`、`viewportReady` 均为 `true`，退出码为 0。
任一必需索引失败时返回非零退出码并列出资源错误；WinUI 启动时也会保留该错误，
不会再将索引加载失败显示为“核心已就绪”。`Build-IMao.ps1` 在最终资源和 DLL 暂存后
执行同一检查。

候选包索引绑定 `manifest.json` 的 SHA-256。仓库固定其 LF 换行；为兼容已有 Windows
检出，运行时先验证原始字节，再尝试仅将 CRLF 转为 LF 的哈希。不会忽略清单内容变化，
也不会绕过索引特征数、词表或载荷校验。

大地图状态检测支持罗盘与右侧成对的圆形加减缩放按钮，并保留连续帧确认。它不需要
已有玩家位置；确认开图后由独立的视口定位器执行全局或带位置提示的搜索。界面识别
成功不代表该区域已有足够地图数据，缺失或未通过验证的区域特征包仍需单独补齐。

`IMaoOptimizationTests` 覆盖上述换行兼容、损坏拒绝和 720p/900p/1080p/1440p 地图 UI
样本。旧视觉回归清单引用的图片可能未随仓库提供，运行前需补齐样本，不能将缺失
图片当作定位通过。

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

## 2026-09-06 识别根因修复

重叠特征包在同场景匹配集合中按坐标和描述子精确去重，避免重复观测破坏最近邻距离比；生成器取消按420像素网格截断到160条的规则，两个梦州包按全部原瓦片重建。扩展搜索保留强几何候选，不再被平移投票取代；离线发布验证对齐附近复核和重新定位条件。

对照实验、回归命令及仍未通过的性能门槛见 [地图识别审计](MapRecognitionAudit_20260906.md)。
