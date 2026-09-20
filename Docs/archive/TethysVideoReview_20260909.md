# Tethys 视频检查

此文记录修复前的诊断。四点校准及修复后的结果见 [TethysCalibration_20260909.md](../TethysCalibration_20260909.md)。

视频 `Wuthering Waves 2026.09.09 - 19.18.57.04.mp4` 长 27.883 秒，2560×1440、60 FPS。大地图左上角明确显示“泰缇斯之底”，所在区域为金枝地。视频包含移动前后两次开图、小地图连续移动以及清楚可读的游戏坐标水印。这次采集有效，暂不需要重录。

## 检查结果

从视频以 2 FPS 抽取帧，排除黑屏和界面过渡后，选取 30 张正常画面并人工核对画面左下角的游戏坐标。坐标仅作为回归真值，不作为匹配器搜索提示。以现有 Tethys 换算 origin=(8593,1382)、scale=1.205 计算预期位置。

- 30 张中有 26 张得到 Tethys 强匹配，但坐标均未达到原有 8 像素误差要求。强匹配相对预期位置的偏差中位数为 X=-1.87、Y=+24.52 个内部地图像素；Y 偏差范围为 +23.73..+24.95。该近似固定偏差需继续定位，不能通过放宽误差阈值处理。
- 同时加载现有资源和 Tethys 诊断包时，6 张大地图帧均因跨场景匹配歧义而拒绝定位。
- 隔离实验只加载原始 `Map_features.imf`、`Map_visual_index.imx`，不加载任何候选包或扩展包，仍能对两次 Tethys 大地图分别得到 44、50 个几何内点。但当前代码把基础库结果统一标成 World。这直接否定了此前“基础库全部属于 World”的假设。

这些数据说明两个待解决问题：基础库需要有依据的场景归属；独立 Tethys 特征坐标与游戏坐标存在系统偏差。该视频覆盖范围较小，不能直接当成四个分散区域的完整校准，也不能证明整个 Tethys 的定位准确率。

## 产物与复现

中间产物位于 `out/independent-maps/video-20260909/`，原视频未修改。

- `video-review.json`：源文件 SHA-256、视频信息及每帧偏差。
- `coordinate-sequence.jpg`：用于人工核对的逐帧游戏坐标。
- `minimap-manifest.json` / `minimap-report.json`：30 帧带真值的小地图回归。
- `viewport-manifest.json` / `viewport-report.json`：现有资源与 Tethys 混合诊断。
- `base-only-manifest.json` / `base-only-report.json`：只加载旧基础库的对照实验。

视觉回归工具新增显式 `diagnosticPacks` 输入，仅在离线进程中加载待验证包，检查二进制来源哈希、索引哈希和场景 ID；不更改生产加载条件或开放配置。大地图样本可通过 `expectedSceneId` 声明画面中可见的场景真值，匹配到 World 也会明确记为失败。

本轮未修改 Tethys 的 `referenceVerification.passed=false`，也未将其加入生产包注册表。真实视频暴露的问题优先于此前合成测试通过的结果，当前仍不能宣称非 World 实机支持已经完成。
