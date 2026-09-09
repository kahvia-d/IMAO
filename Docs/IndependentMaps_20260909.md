# 非 World 地图接入

**最新进展：**四点截图已用于校准，旧库重复特征已按配准证据隔离，Tethys 包通过参考验证并加入运行时清单。下文保留初步实施记录；最终结果与限制见 [TethysCalibration_20260909.md](TethysCalibration_20260909.md)。

本轮补齐按场景隔离的大地图搜索、小地图局部跟踪和补充包恢复。定位结果直接携带场景 ID，不再用中心坐标落在哪块瓦片中推断场景。基础索引的历史分区标签统一按 World 解释，独立地图即使内部坐标重叠也不会合并特征。大地图同一尺度上有多个场景通过几何验证时拒绝发布结果。

局部搜索使用已确认的场景与位置，失败后扩大搜索，最终回退到所有已安装且已开放场景的独立匹配。没有候选瓦片时返回失败，不会误用全量特征。原有几何接受条件、实机参考验证和新区开放条件保留。

## 首个资源包：Tethys

`Assets/FeaturesDatas/KuroTilePacks/Tethys` 已制作公开瓦片特征，场景 ID 为 2、上游 state 为 900。范围依据归档点位坐标除以 100 后换算并向外扩展一圈：瓦片 X=-1..3、Y=-2..1。11 张公开瓦片可用，9 张返回不存在，共提取 7,291 个特征。这不是整个地图地形完整覆盖的证明。

包采用现有场景定义的变换：origin=(8593,1382)、scale=1.205。生成锚点 (25.1,519.5) 来自区域元数据，只用于选择瓦片范围，并非实机测量。`referenceVerification.passed=false`，未加入运行时包清单；当前不能宣称 Tethys 实机定位已经可用。

复现生成：

```powershell
.\scripts\Sync-KuroMapFeaturePack.ps1 -Apply -Scene Tethys `
  -AnchorWorldX 25.1 -AnchorWorldY 519.5 `
  -TileMinX -1 -TileMaxX 3 -TileMinY -2 -TileMaxY 1 `
  -AllowMissingTiles -SkipReferenceVerification `
  -PaddleLib .\third_party\paddle-inference-3.0.0 `
  -OpenCvDir .\third_party\build\opencv-4.11.0
```

非 World 场景现在自动选用对应 state、包名称及运行时坐标变换，读取已通过的外部校准；必须显式传入该场景的锚点，避免沿用 World 的默认值。

单包二进制与视觉索引可单独生成，不重建基础地图：

```powershell
$pack = 'Assets\FeaturesDatas\KuroTilePacks\Tethys'
.\x64\Release\IMaoFeatureConverter.exe "$pack\features.yml" "$pack\features.imf" "$pack\features.imf.manifest.json"
.\x64\Release\IMaoVisualIndexBuilder.exe --pack-only Assets $pack --allow-unverified
.\scripts\Test-KuroMapFeaturePack.ps1 -PackRoot $pack -AllowUnverified
```

`--allow-unverified` 仅允许制作离线索引，不会修改参考验证或场景开放状态。索引与现有基础词典保持一致，并验证输入哈希及写入后的精确往返结果。

## 实机接入剩余步骤

1. 在 Tethys 中选取分散位置，使用 `.\scripts\Start-KuroCaptureAssistant.ps1 -Scene Tethys -Sample 01` 采集。Ctrl+Alt+F9 保存正常画面，Ctrl+Alt+F10 保存人物未移动时的大地图；Ctrl+Alt+F11 记录移动录像。采集不会控制游戏。
2. 按 `KuroSceneCalibrationSamples.md` 的格式填写四点样本，使用 `Set-KuroSceneCalibration.ps1 -Scene Tethys -SamplesPath <样本路径> -Check` 检查变换；应用校准后保持既有瓦片特征坐标系不变；重新验证参考图，不能把游戏校准反向套入瓦片生成。
3. 使用真实游戏坐标和参考截图重新执行同步脚本，传入 `-ReferencePath <截图>`、`-ReferenceFullSnapshot`，移除 `-SkipReferenceVerification`。参考误差必须通过原有 8 像素检查，再制作二进制与索引。
4. 将已验证包加入 `kuro-tile-packs.json`，通过现有资源暂存流程生成包含它的 bundled snapshot。开发版缺省扫描仅加载 World，独立地图必须通过显式资源描述加载。
5. 验证小地图连续移动、打开地图后的标记位置，以及与 World 来回切换。LowerVault、Darkplain、TimeRiftRuins 还需原有 `Approve-KuroSceneRelease.ps1` 的完整开放证据。

## 离线回归入口

```powershell
.\x64\Release\IMaoVisualRegression.exe --scene-self-test
.\x64\Release\IMaoOptimizationTests.exe
.\x64\Release\IMaoVisualRegression.exe . Tests\VisualLocalization\20260906-viewport.json out\independent-maps\viewport.json
```

合成测试让不同场景占用相同内部坐标，覆盖自动选图、局部场景隔离、旧 World 分区标签、无覆盖、空白图、未开放场景和歧义拒绝。它用于证明程序行为，不是游戏准确率或实机验收。

## 本轮验证结果

- Release 的 CoreHost、优化测试、视觉回归工具和单包索引工具编译通过。
- `--scene-self-test`：32 项检查全部通过，含 OCR/开图备用特征窗口的场景隔离、独立包恢复及跨场景同坐标歧义。
- `IMaoOptimizationTests`：全部通过。
- World 大地图回归：8/8 通过，涵盖亮度、分辨率变化及空白/噪声拒绝。
- 小地图回归：29 张处理完成，`acceptancePassed=true`、必需发布 2/2。4 张有标签样本中实际发布 2 张且都正确；其余未标注样本不能据此宣称定位准确。
- `IMao-CoreHost --check-resources x64/Release/Assets`：resourcesReady、visualReady、viewportReady 均为 true。
- Tethys 特征二进制与单包视觉索引写入后精确往返校验通过：7,291 个特征、73 个索引分块。未验证参考仍被普通包检查和 `--verified` 索引入口拒绝。

机器回归报告保存在 `out/independent-maps/`。以上不替代非 World 地图的实机验证。
