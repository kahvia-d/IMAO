# 候选包是否已被梦州区域包取代 · 实测

**问题**：`dreamzhou-curated-locations`（候选包，7 MB）建于"梦州没有区域包"的时代。
`mengzhou-kurotiles`（63 块瓦片，64750 关键点）落地后，候选包是否已经多余？

**结论：不多余。** 在它自己标注的三个锚点上，去掉候选包后有 **2/3 定位失败**。

但本测有重要局限，见第四节——**不能据此断定候选包在生产中是必需的**。

---

## 一、方法

A/B 两次运行，唯一的差别是快照里是否选中候选包。其余完全相同。

```powershell
# 生成两棵树的快照（真实包 + 真实文件清单哈希）
python out\candidate-redundancy\make-snapshots.py out\candidate-redundancy

# 原生校验两份快照
x64\Release\IMao-CoreHost.exe --check-resource-snapshot out\candidate-redundancy\with-candidate.json
x64\Release\IMao-CoreHost.exe --check-resource-snapshot out\candidate-redundancy\no-candidate.json

# 用候选包自己的三张参考图当查询，锚点当期望值
python out\candidate-redundancy\make-manifests.py out\candidate-redundancy
x64\Release\IMaoVisualRegression.exe <repo> out\candidate-redundancy\manifest-with-candidate.json out\candidate-redundancy\report-with-candidate.json
x64\Release\IMaoVisualRegression.exe <repo> out\candidate-redundancy\manifest-no-candidate.json  out\candidate-redundancy\report-no-candidate.json
```

两份快照都通过原生校验：

```
with-candidate: {"error":"","resourcesReady":true,"viewportReady":true,"visualReady":true}
no-candidate:   {"error":"","resourcesReady":true,"viewportReady":true,"visualReady":true}
```

**期望值怎么来的**：候选包的锚点是 world（game）坐标，运行时先用
`MapCoordinate::IdentifyCoorToImgMapCoord` 换算成内部地图像素才参与匹配。
Scene 1（World）在 `scene-calibrations.json` 里**没有** `passed=true` 条目，
所以用编译期 `SceneDefinition`：`origin (2474.0, 1957.0), scale 1.205`。

校验这个换算是自洽的：`(-8519, -292) → (-7791.40, 1605.14)`，
而仓库里既有的 `Tests/VisualLocalization/dreamzhou-west-smoke.json`
对同一地点标注的期望值是 `(-7791.395, 1605.14)`。**逐位吻合。**

---

## 二、结果

| 锚点（world） | 期望 map | A 有候选包 | B 无候选包 |
|---|---|---|---|
| (−6725, −919) | (−5629.63, 849.61) | Strong · 78 内点 · 误差 **0.000** | **Rejected · 0 内点 · 未定位** |
| (−8519, −292) | (−7791.40, 1605.14) | Strong · 97 内点 · 误差 **0.000** | Marginal · 19 内点 · 误差 3.625 |
| (−7690, −176) | (−6792.45, 1744.92) | Strong · 40 内点 · 误差 **1.195** | **Rejected · 7 内点 · 未定位** |

A 组三张全部 `Strong`。B 组：1 张勉强定位（降级为 `Marginal`），2 张完全失败。

---

## 三、覆盖调查（独立于上表）

直接解析各包的真实 `features.imf`，统计锚点附近有多少关键点：

```
=== mengzhou (64750 关键点) ===
   (-6725,-919)  -> map (-5629.6, 849.6)   包围盒内=True  最近 2.6    ≤50:136  ≤100:322  ≤200:914
   (-8519,-292)  -> map (-7791.4,1605.1)   包围盒内=True  最近 24.1   ≤50:22   ≤100:176  ≤200:726
   (-7690,-176)  -> map (-6792.5,1744.9)   包围盒内=True  最近 3.0    ≤50:89   ≤100:244  ≤200:798

=== jinzhou (207205 关键点) ===
   三个锚点 包围盒内=False，最近 6293 / 8304 / 7296
```

**梦州区域包确实覆盖这三个点**，最近关键点只有 2.6 / 24.1 / 3.0 像素。
今州完全不覆盖（这是预期的：今州在 frame 8 正 x 侧，梦州在负 x 侧）。

所以 B 组的失败**不是"梦州包不管这片地"**——它管。原因是别的东西，见下节。

---

## 四、局限（重要，别过度解读）

**A 组的巨大优势主要来自自匹配。**

A 组把候选包的参考图当查询，而候选包的特征就是从这些图里提出来、存进同一个特征池的。
查询因此在池里找到**逐点相同的描述子**——78/97/40 个内点里，绝大多数是记忆命中，
不是泛化能力。所以 **A 组数字不能用来论证"候选包好"**。

B 组是干净的：池里只剩 13 个瓦片包，查询必须真的匹配上瓦片特征。
B 组告诉我们的是**"用候选包自己的截图去查瓦片包"**的结果：2/3 失败。

**但"候选包的截图查不到瓦片包"不等于"实时截图查不到瓦片包"。** 两者是不同来源的图像：

- 候选参考图 = 游戏内小地图截图（`sourceSession` 记录的实机采集）
- 瓦片包特征 = 从 Kuro 官方渲染瓦片里提取的

同一片地形，渲染图和小地图截图的描述子分布并不必然一致。
B 组失败的真正含义可能是**查询图像与瓦片特征的域不匹配**，
也可能是**那两处瓦片特征确实不够**——本测**无法区分**这两者。

因此：**本测能证明候选包不多余，不能证明候选包在实机中仍被需要。**

---

## 五、要定论还差什么

需要**独立于候选包**的梦州实机小地图截图（不是候选包的那三张），
在"无候选包"的树上跑一遍：

- 若仍能稳定定位 → 候选包可以退役，省 7 MB，并简化 frame 8；
- 若失败或降级 → 候选包应保留。

采集要求见 `Docs/DreamzhouCandidateFeaturePack.md`：干净的小地图裁剪，
并独立核实其地图锚点（优先用官方瓦片或成功的图心匹配），OCR 文本不足以作证。
