# 区域按需下载 / 删除 · 设计说明（续篇）

本文是 `Docs/MapRegionRefactor-Handoff.md` 的**续篇**。前一份文档第四节的
"眼下的任务：做无基础库构建"**已经完成并在实机验证通过**，请勿再执行它。
本文记录之后的进展，以及下一件要做的功能。

先自证状态：

```powershell
cd C:\Dcode\WWMAP-TOOLS
git log --oneline -8      # 期望 HEAD = 973f852
git status --porcelain=v1 # 期望无输出
```

---

## 一、前一份文档之后发生了什么

| 提交 | 内容 |
|---|---|
| `3929baa` | 无基础库运行 + 修复"第一个包被算两遍" |
| `973f852` | 测试树不再保留包目录的旧拼写（大小写） |

### 1. 无基础库构建：**实机验证通过**

用户亲自在游戏里跑过、判断定位可用。因此：

- **遗留的融合图集（`Map_features.imf` + `Map_visual_index.imx`）可以退役**
- 程序包 981.9 MB → **831.6 MB**
- 先前设计的 `map-features` 包（"把基础库挪成可下载包"的折中方案）**不再需要**
- 为此落下的 `MapFeatureRoot` 管道（`222ace2`）现在是**惰性的、用不上的**。留着当"将来想把基础库做成可选下载"的接口，或撤掉；撤掉要动 native + managed 共 9 个文件。

判定依据（实测）：用 `scripts/Register-LegacySceneFeatures.py` 对 13 个区域包逐一配准基础库
（247,389 个关键点），并集 **218,615 = 88.4%**；未覆盖的 28,774 个点
**100% 落在已覆盖包围盒内部**（28,772 / 28,774，仅 2 个在外），是散布的孔洞，
不是漏掉的一整片地。八份报告在 `out/legacy-registration/*.json`（本地，被 gitignore）。

### 2. 找到并修复了一个会破坏定位的重复计数

无基础库路径新加的"从第一个包播种"逻辑把第一个包算了**两遍**（计数和追加各一次）。
证据是算术上的硬证据：合并关键点数 894,301，而 13 包 + candidate 应为 856,777，
差额 37,524 **正好等于第一个包 darkplain 的关键点数**。修复后实测 856,777。

副作用是**第一个包之后每个包的特征行基址偏移 +37,524**，视觉索引里记录的行号
指向错误的描述子。

### 3. 目录大小写问题

`New-MapTestTree.ps1` 算对了小写目标名，但 Windows 的 `CreateDirectory` **会静默复用
已存在的旧拼写目录**，于是被删掉的旧场景名（`Darkplain`/`Tethys`/`Lahai`/`RoySurface`/
`BlackShores`）一代代传下去。已修：快照里的包目录**逐段归一到磁盘真实名字**，
纯大小写改名走**临时名两步**（Windows 拒绝直接大小写改名）。

**严重性订正**：那份快照是**构建产物**（未跟踪），全新克隆由 `Stage-UpdateResources.ps1`
按 `kuro-tile-packs.json`（自 `dc5c9bb` 起全小写）生成，所以**不会破坏新克隆**——
这个缺陷只在本地树里自我延续。

### 4. 死目录已删

`Dreamzhou` / `DreamzhouWest`（232.6 MB 真目录 + 两个 junction）已从
`x64\Release\Assets` 与 `out\map-test\Assets` 清除，快照从未引用它们。

---

## 二、接下来要做的功能：玩家在 UI 里按区域下载 / 删除

### 为什么现在做不到

**约束 A（硬）**：客户端只能消费**签名快照**。`ResourceSnapshotService.Load` 要求
`FormatVersion == 2`，且 `UpdateSignature.Verify` 用内置公钥验签。所以客户端
**无法自己合成一个子集快照**交给 CoreHost——签名过不了。
"按需"不能靠"生成只含选中区域的快照"实现。

**约束 B**：运行时注册即加载。`RuntimeFeatureRepository.cpp:230` 会因**任何**已注册包
加载失败或未获运行时批准而中止整个资源加载（表现为"启动核心失败"）。

### 设计：把"可获得"与"已激活"分开

```
签名快照（updates/stable.json）    = 可获得什么（目录清单）
本地选择状态（unsigned，用户数据）  = 激活哪些（按 package id）
送给 CoreHost 的 Current            = 签名快照【过滤】到选择集
```

**关键好处：运行时一行都不用改。** 未选中的包根本不会出现在 CoreHost 收到的快照里，
约束 B 也就永远看不到它们。校验同理——`Load` 本来就是 `foreach (snapshot.Packages)`，
**先过滤即可**，不需要放宽任何校验。

### 强制保留、不可取消的包

| 包 | 原因 |
|---|---|
| `map-data` | `ResourceSnapshotService.cs:194` 要求 `Count(kind=="map-data") == 1`，且 `:203` 要求 `MapDataRoot` 等于它的目录 |
| `map-icons` / `map-features` | 对应根要能解析（若该布局用了它们） |
| `dreamzhou-curated-locations` | 手工精选点位，仅 7 MB |

→ **可选的正好是 13 个区域包**，与"按区域按需"完全对应。

### 两个必须处理的坑

1. **bundled 包的"删除"只能取消选择，省不出空间。** `FindBundledPackage` 会把随程序
   分发的包解析到安装目录（`Assets/...`）。删那些会破坏程序本身。删除动作**只能**针对
   `Root/packages/<id>/<version>`（下载来的副本）。
2. **删除必须拒绝强制包**，否则 `map-data` 一删，`Load` 直接抛"资源快照地图数据包无效"。

### 要动的文件

| 层 | 文件 | 改什么 |
|---|---|---|
| 契约 | `IMao-WinUI.Core/Updates/UpdateContracts.cs` | 本地选择状态（按 `snapshotId` 持久化） |
| 服务 | `IMao-WinUI.Core/Updates/ResourceSnapshotService.cs` | `Current` 按选择过滤；`Rebind`/`MapDataRoot` 仍从强制包推导 |
| 服务 | `IMao-WinUI.Core/Updates/UpdateService.cs` | `EnsureInstalledAsync(packageIds)`（只装子集）、`RemoveAsync(packageIds)`（取消选择 + 删下载副本） |
| UI | `IMao-WinUI/Views/SettingsPage.xaml(.cs)` | 区域列表：名称 / 大小 / 状态（内置·已下载·未下载）/ 下载·删除 + 总下载量 |
| 测试 | `Tests/ResourceUpdates`（已有 `RealOfflineRunner`） | "选两个区域 → 只下这两个 → 删一个 → 快照仍可加载" |

### 实现顺序（每步独立可验证）

1. **选择状态 + 快照过滤**（纯 Core，无 UI）→ 用离线 runner 验证：过滤后的快照能过 `Load`，
   且未选中的包目录**不需要存在**。
2. **`EnsureInstalledAsync` / `RemoveAsync`** → 离线 runner 验证"只下载选中包""删除后仍可加载"。
3. **UI 列表 + 按钮** → ⚠️ 这一步**无法由 AI 做视觉验证**（WinUI/XAML 看不到界面），
   必须由用户在实机上看效果。
4. **实机验收**：取消选中某区域 → 重启 → 该区域不再加载；重新选中 → 只下载它一个。

### 需要用户拍板的两个点

- **候选包（`dreamzhou-curated-locations`）是否也允许取消？**
- **默认选择是"全部 13 个区域"，还是只有某一个子集？**（影响首次安装后的默认下载量）

---

## 三、其余未决事项

- **基础库退役的落地**：删掉 `Assets/FeaturesDatas/Map_features.imf` 与
  `Map_visual_index.imx`，并把 `New-ProgramReleasePackage.ps1:101` 里"程序包必须包含这两个
  文件"的强制要求去掉——**这一步才是字面意义上的"不再打进程序"**。
- `MapFeatureRoot` 管道是否撤掉（见第一节 1）。
- 13 个区域包的窗口重叠问题（早先实测：帧 8 六个包共声明 481 个瓦片引用而实际只有 321 个
  唯一文件，黑海岸 0 张独占）——不影响正确性，但 `map-data` 之外的下载量可以再优化。

---

## 四、环境与命令

见 `Docs/MapRegionRefactor-Handoff.md` 第六、七节（工具链路径、构建命令、原生预检的写法、
配准脚本的代价模型）。要点复述：

```powershell
# 重编
cmd /c "call C:\VSBuildTools-Current\VC\Auxiliary\Build\vcvars64.bat && cmake --build out\build\windows-x64-release --target IMao-CoreHost IMaoResourceSnapshotTests"
# 测试：期望 58 checks, 0 failed
x64\Release\IMaoResourceSnapshotTests.exe
# 交付给用户实机跑的树
out\map-test\IMao-WinUI.exe      # 以管理员运行、游戏开着、16:9
```

`out/diag-nobase-snapshot.json` 与 `out/diag-withbase-snapshot.json` 是排查时用的
**绝对路径 strict 候选快照**，可直接拿来跑 `--check-resource-snapshot`。
`RuntimeFeatureRepository` 现在会通过 `Diagnostics::Record` 记录
`stage=visual-index-composition`（各场景瓦片数），这是排查"某个场景没进索引"的第一手信息。
