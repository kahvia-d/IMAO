# feature-probe

测量工具，用来回答「地图识别与特征包该多大、有多准」这类问题。**这些是只读探针**：它们读已发布的
瓦片与特征包，产出一份报告，不改任何发布物。

## 为什么在这里

这些脚本原本只存在于本机的 `measurements/accuracy-check/`，而那个目录在 `.gitignore` 里——
于是 [`DetectorMigrationDecision_20261008.md`](../../Docs/DetectorMigrationDecision_20261008.md)
引用的脚本在版本库里根本不存在。2026-10-10 把**产出证据的那一版**搬到这里，让结论可复核。

搬过来的是当时的版本，不是最初的版本：`imao_vs_sift.py` 在 10-08 到 10-10 之间从 428 行长到 648 行，
多了 `--gate` 那套判据。

## 文件

| 文件 | 作用 |
|---|---|
| `imao_vs_sift.py` | 主探针。重建一个包的瓦片拼图，逐瓦片抽特征，然后拿**合成裁剪**测定位。`--gate` 选「哪些裁剪算公平的题」：`art`（2026-10-08 原协议，默认，旧数可复现）、`texture`（按结构）、`answerable`（参照集自己必须在框内有 ≥8 个关键点）。报告里带 `queries[]` 逐题诊断 |
| `sweep_per_tile.ps1` | 14 个包跑一遍 `imao_vs_sift.py`，汇总成一份 JSON。`-Gate` 透传，产物另存，可续跑 |
| `coverage_windows.py` | 逐包量「有多少查询尺寸的窗口**存在答案**」，SIFT 与线上 SURF 各一份。这是解释 82.7% 那个数的关键统计 |
| `size_projection.py` | 按索引 section 投影迁移后的体积 |
| `audit_sizes.py` | 更早的体积审计（被 `size_projection.py` 取代，保留是因为角度不同） |
| `detector_compare.py` | 更早的检测器与预算对照（被 `imao_vs_sift.py` 取代，同上） |
| `sweep_all_packs.py` | 更早的逐包关键点/体积估计（被 `sweep_per_tile.ps1` 取代，同上） |

后三个是 2026-10-02 那一轮的脚本，**没有删掉是因为它们与现在这套的切法不同**；用的时候优先用前四个。

## 怎么跑

需要一个带 OpenCV 的 Python。仓库里那个是 `measurements/` 配套的虚拟环境：

```powershell
$py = '.venv-featureprobe\Scripts\python.exe'

# 全部 14 包（约 2 分钟；-SkipExisting 可续跑）
pwsh -File tools\feature-probe\sweep_per_tile.ps1 -OutputRoot out\sweep `
     -SummaryPath out\sweep-summary.json

# 覆盖率
& $py tools\feature-probe\coverage_windows.py --json out\coverage-windows.json

# 单包详查
& $py tools\feature-probe\imao_vs_sift.py --pack tethys --config sift-friend --gate answerable
```

`sweep_per_tile.ps1` 的 `-OutputRoot` / `-SummaryPath` 默认值指向 `measurements/`，用的时候显式指定到
`out/`（该目录也在 `.gitignore` 里）。

## 测量产物仍然不进版本库

这里的脚本进版本库，但它们**跑出来的 JSON 不进**——主 sweep 的产物是几 MB 到几十 MB，而且
可以随时用同样的参数重跑。要留证据就在文档里记参数与结论。

## 已知的坑

- **`--gate` 的默认值 `art` 是故意保留的**：它复现 2026-10-08 那批数（14 包 139/168）。换门禁会让
  数字变化，**不是改进而是换了个问题**。比较检测器之前先读
  [`OpenWork.md`](../../Docs/OpenWork.md) 第 7 节。
- **`sweep_per_tile.ps1` 按自身位置找同目录的 `imao_vs_sift.py`**，所以这一对可以被整体复制到别处。
- Python 的 OpenCV wheels **创建不了 SURF**（专利限制），所以「线上那套 SURF 认得准不准」在这里
  测不了；能测的是它的**覆盖率**（`coverage_windows.py` 靠坐标换算做这件事）。
