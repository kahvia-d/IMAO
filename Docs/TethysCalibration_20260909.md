# 泰缇斯四点校准与资源隔离

## 结果与边界

Tethys（state 900 / scene 2）已加入 `kuro-tile-packs.json`。参考小地图通过原有 8 像素检查，误差 1.526 像素。其余独立地图仍需各自的资源包和实景验证，不能据此宣称全部非 World 地图已经可用。

用户提供的 19:51:30–19:53:17 八张 2560×1440 截图形成四组：游戏平面坐标分别为 (-480,710)、(-403,356)、(-227,843)、(-21,756)。正常画面的第三个水印数值未参与平面校准。四张大地图匹配得到的中心用于拟合；小地图和先前移动视频不参与拟合。

| 检查 | 结果 |
|---|---|
| 四张大地图 | 全部 scene 2；校准最大拟合误差 0.708 px |
| 四张独立小地图 | 3 张强匹配正确，最大误差 3.333 px；1 张拒绝 |
| 先前移动视频的 30 帧 | 26 个强匹配正确；13 次实际发布全部正确；无错误发布 |
| 视频强匹配最大误差 | 3.847 px |
| 视频两次开图、共 6 帧 | 全部匹配 scene 2 |

第 3 张小地图的有效地形不足，几何验证拒绝；同位置的大地图通过。四张独立截图各自缺少第二帧，不能把其强匹配计作已发布位置。以上是保存画面的离线回放，尚不等于运行中的浮层、跨场景切换及各种缩放比例均通过实机验收。

## 根因与修复

旧基础库包含泰缇斯地形，其视觉索引中的最近原点分区不能作为可靠场景标签。将整库统一标成 World 会让泰缇斯同时产生 World 和 scene 2 候选。旧库与公开瓦片包可匹配出 2,059 对特征，平移中位数（新包减旧库）为 (-11.074,21.175) px，配准残差要求小于 2 px。

`Register-LegacySceneFeatures.py` 根据描述子配准与旧图集连通分量生成候选排除记录。经四组截图和视频回放验证，3,818 行旧特征由新包替代。排除记录绑定完整基础 IMF 哈希和替代包特征哈希，只在该包通过正常加载后生效。小地图、大地图及 OCR 邻域取样均使用该记录；它不在运行时按当前位置打补丁，也不改变真实跨场景歧义的拒绝规则。其他未归属旧图集区域仍保留历史行为，后续地图必须继续验证其归属。

四点拟合得到游戏坐标到特征坐标的变换：

```
mapX = gameX * 1.2055468368887101 + 8593.004842739658
mapY = gameY * 1.2055468368887101 + 1408.620887208053
```

瓦片生成坐标系保持 origin=(8593,1382)、scale=1.205。它和游戏校准是两个不同变换；把游戏校准再次用于生成瓦片会重建同样的误差。构建脚本现在保留既有瓦片坐标系，并通过独立 `referenceCoordinateTransform` 检查游戏参考图。`--verify-only` 可以验证已有特征，不重新提取或覆盖它们。

## 证据与复现

包内 `calibration-evidence.json` 保存坐标、截图路径与 SHA-256，以及校准和独立验证统计；`reference-minimap.png` 保留实际通过检查的小地图。源截图和视频未修改。中间报告位于 `out/independent-maps/screenshots-20260909/` 与 `out/independent-maps/video-20260909/`。

```powershell
python scripts/Register-LegacySceneFeatures.py Assets/FeaturesDatas/Map_features.imf Assets/FeaturesDatas/KuroTilePacks/Tethys out/independent-maps/registration.json
./scripts/Test-KuroMapFeaturePack.ps1 -PackRoot Assets/FeaturesDatas/KuroTilePacks/Tethys
./x64/Release/IMaoVisualRegression.exe --scene-self-test
./x64/Release/IMaoVisualRegression.exe . out/independent-maps/video-20260909/minimap-runtime-manifest.json out/independent-maps/video-20260909/minimap-runtime-report.json
```

注册脚本只输出候选记录，不自动修改生产包。新基础库哈希或替代包特征变更后，旧排除记录不能沿用。

## 最终构建与回归

- Release 编译：IMao-CoreHost、IMaoVisualRegression、IMaoOptimizationTests、KuroMapFeatureBuilder 完成。
- 多场景合成用例 36 项通过，包含旧行替换、大地图/小地图/OCR 隔离，以及真实跨场景重复图像继续拒绝。
- OptimizationTests 退出码 0；World 大地图 8/8；World 小地图 29 帧回归 acceptancePassed=true（仅 4 帧带位置真值，不能称 29 帧全部定位准确）。
- 去掉 diagnosticPacks 后，泰缇斯四张大地图与视频回放仍通过；独立四张小地图仍是 3 个正确强匹配、1 次拒绝，无截图第二帧发布证据。
- x64/Release 资源已通过 Stage-UpdateResources 装配；包含 Tethys 的绝对路径快照经 CoreHost --check-resource-snapshot 检查，resourcesReady、visualReady、viewportReady 均为 true。
- 未提交或发布；尚未运行游戏中的新版本浮层验收。

## 20:18 浮层截图的点位投影核对

用户指出海面宝箱未落在地形上。对实际浮层截图重放，地图定位为 scene 2，31/31 几何内点。将归档标记数据通过当前校准和实际视口比例投影，六个小型信标均与游戏自身图标吻合（逐个裁剪目视核对，非现场宝箱正确性证明）。这否定了本次排查中“游戏校准被误用于标记，导致全图统一偏移”的初始假设。

所指最近宝箱为朴素奇藏箱 1290809676708536320，来源坐标 (-428.07,1062.69)，描述“位于礁石上”。计算投影约 (993.41,893.41)，与截图实际宝箱图标位置一致。现有截图不能判断礁石未在大地图绘出还是源点位有误；不据此平移全图或修改该点。当前 floorId 为空、level=0，也不足以判定它属于其他图层。

诊断记录与六个信标投影核对图在 out/independent-maps/marker-review-20260909/。视觉回归输出新增 captureCorners，供复核完整的视口投影。本次没有修改运行时坐标或点位数据。

用户随后实地确认了礁石，并确认另一处海面标记位置确实存在宝箱。原先对这两处点位的怀疑已获得用户现场反馈支持；不应为让标记贴合二维地图色块而改动它们。
