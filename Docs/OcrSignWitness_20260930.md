# 坐标读数丢负号：为什么位移预算拦不住，以及现在由谁作证

2026-09-30。梦枢天罗实机日志 + 四张校准截图的离线复现。

## 一、现象（全部有据）

`events-20260930.jsonl`，19:52:21–19:52:32 共 11 秒里发布了 **4 个 y 镜像的坐标**：

```
19:52:21  world=-415,205   reason=confirmed  jumpUnits=965.400649  agreements=1
19:52:23  world=-415,204   reason=confirmed  jumpUnits=1.873526    agreements=2
19:52:25  world=-420,-203  reason=confirmed-no-lock               ← 真值
19:52:26  world=-425,200   reason=confirmed  jumpUnits=10.770330   agreements=3
19:52:28  world=-428,196   reason=confirmed
19:52:32  world=-431,-189  reason=confirmed                        ← 真值
```

真值是 y ≈ **−205**，依据不是"看起来像"：那四张校准截图上的数字是肉眼读的，而它们与大地图
定位拟合到 **0.53 px**——一个 y 符号差 418 单位，拟合误差会是约 500 px。

同几秒里图像锚定那一路一直是对的（`minimap-near-items playerROC=-500.905865,-247.435427`，
`minimap-render-position mode=authoritative fixAgeMs=50`），所以**镜像误差是 494 地图像素、
而正确答案当时就在手边**。

## 二、原来的判断错在哪（这一条比修复本身重要）

我（以及 `OcrCoordinateGate.h` 开头那段注释）原来都认为这种错发生在"**没有锁**"之后：
锁没了，读数之间"连续两帧一致"就成了唯一判据，而两次一起错是必然的。
但这一帧的 `reason=confirmed` 说明**它当时有锁**，而且是走"有先验"那条分支发布的。

真正的原因是两个判据都被**锁龄**放大了：

| 判据 | 公式 | 19:52:21 的实际值 |
| --- | --- | --- |
| `OcrCoordinateGate::JumpBudgetUnits` | `max(120, 距锁秒数 × 100)` | 锁是 63 秒前开大地图时确认的 ⟹ **6300** |
| `commitVisualPosition` 里的提交闸门 | `max(120, 距锁秒数 × 100)` | **6300** |

玩家在 63 秒里真的走了约 900 单位（跨了大半张图），所以 965 单位的镜像误差**落在预算内**。
换句话说：**"锁旧 + 走远了"时，位移预算等于没有**——而这正是玩家传送到新地图、或者开完图
长途飞行之后的状态，也就是新地区第一次进去时最常见的状态。

另一个本该拦住它的判据是 `CoordinateTrust::IsMirrorOf`（"候选正好是预测位置的镜像"，
注释里写明这是丢负号与瓦片错位的精确签名），但它只在 `hasTrend` 为真时生效，而
`FitAt` 需要最近 30 秒内 ≥6 条**链条连续**的记录——900 单位的真实位移本身就是链条断点，
所以那一刻没有趋势可用。

## 三、两条预处理路线都不可靠（推翻了代码里的假设）

`IdentifyWorldCoordinates.cpp` 原来的注释写着：低对比度坐标文字"在 contrast/binary 路径上
最容易丢负号，TopHat 路径就是为保住这些细笔画做的"，运行时因此一帧只跑 TopHat 一条路线。

用真实截图跑 `IMaoCoordinateRegression`（`fullSnapshot: true`）的结果：

```
p1 真值 -413,-209,15   contrast ✅ -413,-209,15      tophat ❌ -413,209,15 :   （丢掉 y 的负号）
p2 真值 -179,281,-23   contrast ❌ +179,281,-23      tophat ✅ -179,281,-23 :  （负号读成加号）
p3 真值 409,390,16     contrast 409,390,1620         tophat 409,390,16 2       （只有 z 不同）
p4 真值 490,-124,25    contrast 490,-124,252         tophat 490,-124,25 2
```

**两条路线都会认错符号，只是错在不同的帧上**，而且运行时恰好跑的是 p1 出错的那一条。
所以修法不是"换一条更准的路线"，而是**让符号由两个独立来源互相作证**。

## 四、现在的规则

### 1. 没有新鲜先验的帧：两条路线互证（`CoordinateCandidateParser::Corroborate`）

`App` 在没有新鲜锁时把 `CoordinateRecognitionRequest::crossCheckRoutes` 置真，识别器于是
跑 contrast + tophat 两条路线，**只保留两条路线都读出来的坐标（x 与 y 完全相同）**。

- 为什么只比 x、y：p3/p4 显示同一帧里 z 经常对不上（把右侧的时钟文字读进来了）。
- 为什么要求完全相同而不是"符号一致"：符号一致会放过"两条路线都读错同一个数字"，
  而这里要挡的正是符号本身（`-413,-209` 与 `-413,209` 不会互相作证）。
- 代价：这种帧多一次推理（约 25 ms），而且**两路不一致时整帧丢弃**（p1、p2 都会丢）。
  这是有意的取舍：那种帧宁可不出坐标。丢了多少由新日志 `ocr-route-agreement`
  的 `kept=` / `dropped=` / `primary=` / `witness=` 统计。
- 诊断工具不受影响：`RecognizeSnapshotForDiagnostics` / `RecognizeCropForDiagnostics`
  不打开这个开关——它们存在的意义就是分别看清每条路线读出了什么。

### 2. 有新鲜视觉定位时：镜像签名否决（`OcrCoordinateGate::MirrorsVisualPrior`）

`App` 维护一份**只由图像匹配写入**的定位（`App.h` 的 `lastVisualFixMapCoordinate` /
`lastVisualFixAt`，1.5 秒内算新鲜），读数候选若正好是它关于地图原点的镜像，就不发布
（新日志 `coordinate-publish-rejected reason=sign-mirror`）。

- 为什么必须是"只由视觉写入"：`lastPlayerImgMapCoordinate` 每次提交都会动，包括读数自己；
  拿它当参照的话，一次丢负号的发布就会让真值看起来是镜像——这正是 `CoordinateTrust.h`
  里"不能用当前位置当参照"那条教训。
- 为什么这条判据不会自我锁死：视觉定位是图像匹配，读数永远写不进它。
- 容差 60 单位（与 `CoordinateTrust::kMirrorToleranceUnits` 同值）：镜像误差是 2×|坐标|
  （这次 410 单位），而 60 单位 ≈ 0.6 秒飞行。

### 3. 判据之间是互补的

| 情形 | 谁在拦 |
| --- | --- |
| 有新鲜锁 | 位移预算（原本就有效） |
| 锁旧 + 走远了（**这次的漏洞**） | 视觉定位的镜像签名 |
| 视觉定位也停了（薄特征区） | 双路线互证 |
| 两路一起错 | **没有判据**——这是残留风险，见下 |

## 五、验证

- `IMao-Core/tests/OcrRouteAgreementTests.h`（新增，跑在 `ctest` 里）：用上面四条真实读数原文
  钉住互证规则。
- `OcrCoordinateGateTests.h`：新增 5 条，其中一条专门断言"**单靠位移预算会放过它**"
  （`bareJump` 在 6300 的预算内被接受），另一条断言真值读数仍然能发布（这条规则不和真值作对）。
- **对照实验**（照 K37 的做法，证明测试真的在钉东西）：把 `mirrorToleranceUnits` 改成 0、
  把互证的比较短路成恒真，重建后正好这 4 条断言变红，其余不受影响；恢复后全绿。
- 回归语料：`Tests/CoordinateRegression/manifest.json` 加了 `mengshutianluo-p1-190342` /
  `mengshutianluo-p2-190414` 两条（`fullSnapshot: true`），跑出来**逐字符复现**上面的读数。

## 六、写 fixture 时踩到的两个坑（给以后做这件事的人）

1. **`RecognizeCropForDiagnostics` 不是"把这张裁剪图喂进去"**：它先把图缩到高 44、宽
   `min(200, …)`，贴进一张合成的 1600×900 画面的 `(20,856,…)` 处，然后 `Recognize` 再按
   ROI `(20,865,140,35)` 裁一次——**右侧约 20% 被丢掉**。224×56 的读数裁剪图经它一跑，
   `-413,-209,15` 会变成 `-413,-209,1`。所以裁剪图 fixture 测的不是运行时那条路径。
2. **GDI+ 的 `DrawImage` 不是逐像素拷贝**：把真实读数区 1:1 画进一张全尺寸黑帧，出来的
   读数仍然是被截断的 `-413,-209,1`。想造"忠实的合成帧"必须先解决颜色管理/重采样，
   不值得——**唯一忠实的 fixture 就是原始整屏图**（`fullSnapshot: true`）。

于是这两条 fixture 指向 `map-regions/references/mengshutianluo.png` 与
`mengshutianluo-p2.png`（整屏图，按惯例 gitignore）。语料里原有 61 条指向
`x64/Debug/Diagnostics/`，在没有那份诊断输出的机器上会被记成 `missing` —— 这套语料
本来就是机器本地的，能跑的那两条现在是这两条新的。

## 七、残留风险（明确写下，别当成已解决）

- **两条路线一起错**时互证会通过。这次四张图里两路是**反相关**的（各自在不同的帧上出错），
  但没有证据保证永远如此：同一个模型、同一张渲染，系统性错误仍可能同时命中。
  真正独立的第二来源是视觉定位（规则 2），所以两条规则都要留。
- **盲区帧会丢坐标**：追踪停摆 + 两路不一致 ⟹ 这一帧没有读数。代价是定位慢几帧，
  换的是不发镜像位置。`ocr-route-agreement` 的 `dropped=` 就是它的损失统计。
- **`FitAt`/镜像判据仍受链条断点限制**：这次没有改动 `CoordinateTrust`，
  规则 2 是绕开它而不是修它。要不要让趋势判据在"长距离真实位移"后重建（而不是判定为断点）
  是另一个题目。
