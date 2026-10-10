# 程序更新的磁盘占用：三份完整快照的成因、实测，与"复用改硬链接"方案（2026-10-10）

> **状态：§5（硬链接复用）、§6.1（版本保留策略）、§6.2（语义锁）、§6.3（同卷回退）已于 2026-10-10 实施；
> §6.4（一次性迁移）未实施。** 本文记录实测证据与决策；已落地的部分在下面标出。
> 相关的现行文档是 [`ProgramUpdates.md`](ProgramUpdates.md)（程序自更新）与
> [`ProgramShardIncrementalPlan.md`](ProgramShardIncrementalPlan.md)（分片增量）。本文是它们的续篇：
> 分片解决了**传输**的增量，本文处理**磁盘**的增量。

## 一、现象：一个玩家版安装目录到了 10.14 GB

玩家（作者本人机器，`C:\Dapps\IMao`）报告安装目录体积异常。实测 10,186 个文件、**10.14 GB**：

| 位置 | 体积 | 说明 |
|---|---|---|
| `ProgramUpdates\versions\` | **8.60 GB** | 6 个完整历史版本副本 |
| 根目录程序文件 | 0.59 GB | 首次手动安装（2026.10.3.1） |
| 根目录 `Assets\` | 0.94 GB | 该次安装自带的地图资源（瘦身前） |

六个版本目录：`2026.10.4.1` / `10.4.2` / `10.6.1` / `10.7.1`（各 ~1,580 MB）、`10.7.2`（1,582 MB）、
`10.8.1`（904 MB）。而 [`state.json`](../IMao-WinUI.Core/Updates/ProgramUpdateStore.cs) 只引用
`current = 2026.10.8.1` 与 `previous = 2026.10.7.2` —— **另外 4 个是纯泄漏，合计 6.17 GB**。

这一部分不是设计问题，是**缺少 GC**：全仓库搜索确认 `IMao-WinUI.Core\Updates\` 里
没有任何裁剪版本目录的代码，唯一的删除是 `RemoveScratch`，只针对 `staging\`（并且源码注释
明确写了 "only remove this operation's own unpublished staging tree, never a versions directory"）。
清掉这 4 个目录后安装体积降到 **3.97 GB**。

**但 3.97 GB 仍然偏高**，而剩下的部分不是泄漏，是设计的直接结果。本文处理的是这一部分。

## 二、三个版本目录各自是什么

`Current` 为空字符串代表**首次手动安装的程序本身**，即安装根目录：

```csharp
// ProgramUpdateStore.cs:11
public string Current { get; set; } = ""; // Empty means the original, manually installed program.
// ProgramUpdateStore.cs:71
public string AppDirectory(string id) => id.Length == 0 ? InstallRoot : Path.Combine(VersionRoot(id), "app");
```

根目录那份因此同时承担四个角色，都不是冗余：

- **启动入口**：`IMao-Launcher.exe` 必须留在根目录；
- **引导态**：首次安装后尚未更新时，`Current == ""` 就是运行的那一份；
- **首次更新的复用源**：`PrepareAsync` 里 `reuseRoot = state.Current.Length > 0 ? AppDirectory(state.Current) : InstallRoot`（`ProgramUpdateStore.cs:130`）；
- **最后回落**：`BeginLaunchAsync` 在 current 与 previous 都验签失败时回落到 `""`（`ProgramUpdateStore.cs:476-479`）。

所以三份的角色是「运行中 / 回退目标 / 原始恢复版本」——这是有意为之的冗余，
[`ProgramUpdates.md`](ProgramUpdates.md) 第 41 行也写明了「本版保留原始程序、当前版本、
上一成功版本及其他历史安装目录，不自动清理它们」。

真正的问题在这一句：

```csharp
// ProgramUpdateStore.cs:384-410  TryCopyVerifiedAsync
await using (var input = new FileStream(source, ..., FileAccess.Read, ...))
await using (var output = new FileStream(target, FileMode.CreateNew, ...))
{
    // 边读边算 SHA-256，逐字节写一份新的
}
return Convert.ToHexString(hash.GetHashAndReset()).Equals(expected.Sha256, ...);
```

**系统在动手之前已经精确知道哪些文件逐字节相同**——`TryReuseShardAsync`（`ProgramUpdateStore.cs:348-382`）
拿新旧两份**签名清单**逐文件比 `size` + `SHA-256`，通过的就从当前版本复制。这个信息只用来省流量
（跳过分片下载），**不用来省磁盘**：复用的实现是"读出来再写一份"。

一句话：**同一份增量信息只用了一半。**

## 三、实测：重复的量有多大

把三棵树按类别拆开、逐文件比 `相对路径 + SHA-256`（本次排查期间，在清理后的 3.97 GB 上实测）：

| 树 | 程序 | 资源 | 合计 |
|---|---|---|---|
| 根（2026.10.3.1） | 612.2 MB | 966.3 MB | 1,579 MB |
| `versions\2026.10.7.2` | 615.0 MB | 966.4 MB | 1,581 MB |
| `versions\2026.10.8.1` | 615.1 MB | 287.8 MB | 903 MB |
| **表观合计** | 1,842.3 MB | 2,220.5 MB | **4,063 MB** |
| **按内容去重后** | 993.3 MB | 1,155.4 MB | **2,149 MB** |

**1,914 MB 是逐字节重复的。**

### 3.1 程序可以干净地分成两层

| 层 | 体积 | 跨版本行为 |
|---|---|---|
| **冻结层**（第三方原生库 + .NET 运行时 + WinUI） | **~425 MB** | 逐字节相同，全版本共享一份 |
| **自有构建产物**（Launcher / KuroSyncBridge / CoreHost / 自家 DLL） | **~190 MB** | **每版必变** |

实测依据：根 ↔ 10.8.1 程序侧相同 **422.4 MB**；10.7.2 ↔ 10.8.1 相同 **426.5 MB**。
跨 6 个版本，这个共享集合稳定在 ~425 MB。

三棵树逐字节完全相同的文件共 **1,176 个 / 521.1 MB**：

| 类 | MB | 文件数 |
|---|---|---|
| 程序顶层文件 | 422.4 | 546 |
| `Assets\KuroMapIcons` | 35.4 | 540 |
| `Assets\models` | 22.9 | 10 |
| `Assets\Fonts`（一个 `msyh.ttc`） | 18.8 | 1 |
| `Assets\KuroMap` | 16.7 | 30 |
| `Assets\th.jpg` + `WindowIcon.ico` | 3.6 | 2 |
| `Assets\FeaturesDatas`（未变部分） | 1.3 | 46 |

冻结层最大的成员：

```
115.5 MB  paddle_inference.dll
 88.4 MB  mklml.dll
 45.1 MB  mkldnn.dll
 25.1 MB  Microsoft.Windows.SDK.NET.dll
 18.8 MB  Assets\Fonts\msyh.ttc
 15.7 MB  Assets\models\PP-OCRv5_mobile_rec_infer\inference.pdiparams
 14.4 MB  Microsoft.ui.xaml.dll
 12.6 MB  System.Private.CoreLib.dll
```

注意资源侧：`KuroMapIcons + models + Fonts + KuroMap = 93.8 MB` 是长期冻结的（图标、字体、
OCR 模型、内置点位数据），**只有瓦片包会大改**。

### 3.2 不共享的部分

`10.7.2 → 10.8.1` 变化的文件：**264 个 / 377.4 MB**

| 类 | MB | 文件数 |
|---|---|---|
| `Assets\FeaturesDatas`（瓦片包重压缩） | 188.8 | 253 |
| **程序** | **188.6** | **10** |

**程序侧只有 10 个文件在变**：

```
65.4 MB  KuroSyncBridge.exe
65.0 MB  IMao-Launcher.exe
35.7 MB  IMao-CoreHost.exe
20.3 MB  opencv_world4110.dll
+ 6 个小文件
```

### 3.3 资源的行为不是线性的

| 对比 | 资源相同量 | 含义 |
|---|---|---|
| 根(10.3.1) ↔ 10.7.2 | **966.1 / 966.3 MB** | 跨 **5 次程序更新**资源几乎一个字节没动 |
| 10.7.2 ↔ 10.8.1 | 99.0 / 966.4 MB | 唯一那次瓦片包整体重压缩，几乎全换 |

**资源平时基本冻结，偶尔整体重写一次。** 这决定了稳态估算里资源应作为常量、而非按每次累加。

> ⚠️ 这里只有一个"5 次全同"样本和一个"1 次全换"样本，中间态没有数据。
> 稳态估算按"平时不变"处理，属于保守假设。

## 四、被评估并否决的两条路

### 4.1 原地替换 + undo log（MAA 的做法）

MAA（MaaAssistantArknights）的实际做法（本机 6.19.0 的 `MAA.Updater.exe` 字符串 +
`debug\pending-update-applier.log` 的 46 次真实运行记录 + `src/MaaUpdater/main.cpp` 源码）：

1. GUI 下载增量包、解压到 `NewVersionExtract\`，写 `maa-pending-update-<guid>.json`（4 个字段：
   `packageType` / `removeList` / `moveList` / `relaunchArgs`），启动 `MAA.Updater.exe` 后自己退出；
2. 更新器等父进程退出，取命名互斥体 `MAA_<sha256(路径)>`（**与 GUI 的单实例锁同名**）；
3. 对每个将被替换/删除的文件，先移到 `.old\<相对路径>`；
4. 装新文件 `InstallFileAtomic`：`ReplaceFileW` → `.tmpinstall` + `MoveFileEx` → 重试 10 次；
5. 失败则 `Rollback: restoring <x>`（**仅同一次运行内、且仅恢复"备份还在而目标已不在"的条目**）；
6. 成功后写 `pending-update-success.txt`，MAA.exe 下次正常退出时删掉 `.old`。

**磁盘结果与硬链接方案完全相同。** 按本机真实数字：活的那份 903 MB + 被替换的旧字节
1,056 MB = **1,959 MB**；硬链接方案留两份 = 1,581 + 378 = **1,959 MB**。原因很直接：
10.8.1 那次重压缩动摇了整棵树的 **67%**，"只存增量"在重写型更新上并不省。

代价是放弃四样保证：整树校验闸门（原地改写期间树既不等于旧版也不等于新版）、
原子发布、`ProgramUpdates.md` 的「运行中的程序文件从不覆盖或搬走」、以及零写入回退。
而且 MAA 为此另外放弃了**签名校验**（更新器与 GUI 全程无任何哈希/签名检查，信任完全等于
HTTPS + 托管方）和**跨版本回退**（没有任何跨轮次从 `.old` 恢复的路径）。
这两样恰好是本项目相对 MAA 的全部优势。

MAA 能这么干是因为它的载荷只有 542 MB、每次更新中位数只改 24 个文件。同一套做法搬到本项目，
崩溃窗口会从"几秒内改 24 个文件"放大成"几十秒内重写上千个文件"。

**结论：不采纳。**

### 4.2 git 更新（okww 的做法）

ok-wuthering-waves 的实际做法（本机 `C:\Dgames\okww` + 仓库源码）：

- `ok-ww.exe`（9.9 MB）只是启动器；真正的程序在 `data\apps\ok-ww\`；
- 其中 `repo\` 是一个**独立的 git 仓库**（`ok-ww-update` / `ok-ww-update2`），
  `working\` 是运行副本，`python\`（1,274 MB）是内嵌 CPython；
- 更新 = `git fetch` + `git checkout <tag>`，版本列表就是 `refs/tags`
  （本机 17 个 tag，与 `app.json` 的 `available_versions` 完全一致）；
- CI（`build.yml`）打 tag 时把主仓库同步推送到三个更新远端并打同名 tag。

git 一个原语同时给了增量（pack 对象 delta）、完整性（内容寻址）、回退（checkout 旧 tag）
与版本身份确定性。**但它的前提是程序为解释型源码**——更新载荷只有 45 MB 工作树，
可以逐文件签出。本项目的程序是编译 .NET + 大体积原生 DLL（`paddle_inference.dll` 115 MB 等），
正是字节的大头，且不能热替换。

**结论：不适用。** 可借鉴的是它的思路（见第五节），不是它的机制。

## 五、方案：把"复用"从复制改成硬链接

### 5.1 做法（✅ 2026-10-10 已实施）

`TryReuseVerifiedAsync`（原 `TryCopyVerifiedAsync`，`ProgramUpdateStore.cs`）先建硬链接，失败则复制，
**两条路径落地之后都对目标文件算一次 SHA-256**：

```csharp
if (!UpdateStorage.TryHardLink(target, source))
{
    await using var input = new FileStream(source, FileMode.Open, FileAccess.Read, ...);
    await using var output = new FileStream(target, FileMode.CreateNew, FileAccess.Write, ...);
    await input.CopyToAsync(output, 131072, ct);
}
// 然后读 target 算哈希，与签名清单里的 sha256 比对；不符则删掉 target 并返回 false
```

判定复用哪些文件仍然由 `TryReuseShardAsync` 拿两份签名清单比 `size` + `SHA-256` 决定，
**不需要新增任何"冻结层"或"资源"的分类表**：被链接的集合自动等于"这次没变的那些文件"。
实测观测到的 521 MB 是结果，不是规则。

为什么哈希的是**落地文件**而不是源文件：建链之后两个名字就是同一份字节，复制之后只有副本存在——
两种情况下 target 都是权威。哈希仍然只承担一个职责：**让一个本地损坏的文件只拖累它所在的那一个分片**，
而不是让整次准备失败。真正保证发布物可信的是 `PrepareAsync` 在 `AssembleAsync` 之后对**整棵树**跑的
`ProgramPackageValidation.VerifyDirectoryAsync`，所以链接不可能把一个副本会拦下的字节带过发布闸门。

新增的暴露面要写清楚：链接之后，**对版本目录里的文件做原地修改会同时损坏两个版本**。
版本目录按设计不可变（`ProgramUpdates.md`：「运行中的程序文件从不覆盖或搬走」），而且唯一已知的原地写入者
`imgui.ini` 已在 2026-10-01 之后改为写在 `%LOCALAPPDATA%`（`IMao-Core/src/ImguiDraw/ImGuiOverWindows.cpp`），
何况它不在任何清单里、永远不会被链接——**只有签名清单声明的路径才会被链接**。
即便如此，失败模式也仍是"准备失败、运行中的程序不受影响"，与全部复制时一致。

### 5.2 为什么不破坏任何现有保证

全部链接检查用的都是 `FileAttributes.ReparsePoint`：

```csharp
// UpdateStorage.cs:98-109
internal static void RejectLink(string path)
{
    ...
    if ((File.Exists(node) || Directory.Exists(node)) && (File.GetAttributes(node) & FileAttributes.ReparsePoint) != 0)
        throw new InvalidDataException("资源目录不能包含符号链接或目录联接：" + node);
    ...
}
```

**NTFS 硬链接不是 reparse point**，所以 `VerifyDirectoryAsync` / `VerifyFileAsync` 全部照常通过；
`SHA-256` 校验也照常通过（内容相同）。
发布侧同样不受影响：`tools/UpdatePublisher/Program.cs:435` 拒绝的是 ReparsePoint，
且 ZIP 格式里没有硬链接概念。

整树校验、原子发布（`Directory.Move`）、签名清单、指针式回退**一个都不动**。
网络行为也不变——这只影响本机复用，不影响分片下载。

### 5.3 收益（在本机 4,063 MB 上实测）

| 方案 | 稳态磁盘 | 整树校验 | 原子发布 | 签名 | 运行文件不被碰 |
|---|---|---|---|---|---|
| 现状 | 4,063 MB | ✓ | ✓ | ✓ | ✓ |
| **硬链接复用** | **2,149 MB** | ✓ | ✓ | ✓ | ✓ |
| 原地替换 + undo log | 1,959 MB | ✗ | ✗ | ✓ | ✗ |

**回收 1,914 MB。**

关键的一点：**硬链接之后，根目录那份的边际成本从 1,579 MB 降到约 190 MB**（它与后续版本
共享那 425 MB 冻结层）。所以之前"删掉根目录省 1.5 GB"的结论反过来了——**留着它更划算**。
`previous` 同理，从 1,581 MB 降到 ~190 MB。**"三份完整副本"这个抱怨因此消失，而不是靠删副本解决。**

### 5.4 新装玩家的稳态估算

2026-10-10 实测干净重装最新版（2026.10.8.1）：

| 项 | 体积 |
|---|---|
| 程序 | 615 MB |
| 内置资源（含瘦身后瓦片包 187.8 MB） | 288 MB |
| **安装目录合计** | **903 MB**（1,459 个文件） |
| 下载的完整包 `IMAO-v2026.10.8.1-win-x64.zip` | 446.9 MB（可删） |

稳态公式（N = 保留的版本数）：

```
安装目录 ≈ 425 + 190×N + 资源并集   (MB)
```

| N | 程序 425+190N | 资源 | 安装目录 | AppData | 玩家总计 |
|---|---|---|---|---|---|
| 1 | 615 | 288 | 903 | ~150 | **1.03 GB** |
| 2 | 805 | 288 | 1,093 | ~180 | **1.24 GB** |
| **3**（推荐） | **995** | **288** | **1,283** | **~200** | **1.45 GB** |
| 5 | 1,375 | 288 | 1,663 | ~200 | **1.82 GB** |

- **每多保留一个版本约 +190 MB**，而不是 +900 MB；
- 资源按常量 288 MB 计（三版共享）；一次大地图整体重写会让它临时涨到 ~576 MB，
  等旧版本被 GC 后回落；
- `AppData` 那一列是 `%LOCALAPPDATA%\IMao-WinUI`：设计内的 `ResourceUpdates` 约 150 MB
  （`mounts` 34.4 + `packages` 103.6 + `snapshots` 9.3），其余是随时间累积的日志/诊断/攻略缓存。

**对照本机清理前的三份布局 4.06 GB → 1.28 GB，降 68%。**

> 与最初设想的关系：最早的想法是「一份活的 + 增量替换 + 被替换的旧字节当回退」，稳态
> 903 + (190 + 少量资源) ≈ **1,120 MB**。硬链接方案**留两份就是 1.12 GB，完全等价**；
> 多花 230 MB 留第三份，换来"任意保留版本都能零写入回退"。

## 六、配套改动

### 6.1 版本保留策略（✅ 2026-10-10 已实施）

`ConfirmHealthyAsync`（`ProgramUpdateStore.cs`）在状态提交**落盘之后**调用 `PruneUnreferencedVersions`，
删除 `versions/` 下既非 `Current`、也非 `Previous`、`Pending`、`Trial` 的目录。之所以放在提交之后：
提交正是"被替换的那一版不再可选中"的时刻；其它任何状态迁移都还指着它马上要保留的东西。约束：

- 持有 `UpdateStorage.LockAsync(Root)`；
- 尊重 `Pending` 与 `Trial`（绝不删这两个指向的目录）；
- **永不触碰安装根目录**——它不在 `versions/` 之下；
- 尽力而为，删不掉（文件被占用）就留给下次，绝不阻断启动；
- 候选来自**目录列举**、按**名字**与状态比对（`OrdinalIgnoreCase`），**不用任何状态值拼路径**——
  损坏或恶意的版本 id 无法操纵删除目标；
- `versions/` 或某个版本目录是符号链接/目录联接时整块不动（`RejectLink`）；
- `state.Versions` 是防重放记录、与目录生命周期无关，保持不动。

验证：`Tests/ProgramUpdates` 新增三条用例（提交后清理无人引用的版本 / 四个状态槽各自保留 /
残留被占用时不阻断提交），合计 **55 项通过**；把调用去掉做对照，失败点正是
`the version no record names is gone` 一条。`OpenWork.md` 4.8 已了结，`ProgramUpdates.md` 的
「不自动清理」段落已改写。

⚠️ **存量安装已经积下的孤立目录要等下一次成功更新提交时才会被收走**；本机那份 6.17 GB 是手工清的。

保留范围暂定 `Current` + `Previous` + 未决候选。等 §5 落地后可以**反过来放宽**——多留几个版本当回退目标，
边际成本只有"独有字节"，那是净功能增益而不是妥协；在硬链接之前放宽只会多占整份副本。

### 6.2 语义锁：显式声明允许硬链接（✅ 2026-10-10 已实施）

原来"硬链接能通过校验"只是 `RejectLink` 只认 `ReparsePoint` 的**副产品**。现在它是显式契约：

- `UpdateStorage.RejectLink` 的文档注释写明：判据就是 `FileAttributes.ReparsePoint`，**硬链接不是
  reparse point**，更新系统**有意**使用它；并直接写出禁令——**不要收紧成链接计数**，因为已经更新过的
  安装按设计就持有共享文件，那条检查会让它们全部无法启动，而**没有任何更新能修复一个起不来的客户端**。
- 三条新用例把契约钉死：已链接的文件必须能通过 `RejectLink`；持共享文件的版本必须通过
  `ValidateInstalledAsync`（启动器每次启动跑的那套：签名 + 完整文件清单 + 逐个摘要）；
  目录联接（junction，无需提权即可创建）必须**仍然被拒绝**。
- 链接数由 `GetFileInformationByHandle` 读取（测试侧 `HardLinkProbe`）：共享文件与复制文件的**大小和
  摘要是完全一样的**，只有链接数能区分它们——所以这是唯一不会偶然通过的断言。

### 6.3 同卷检测与回退（✅ 2026-10-10 已实施）

`UpdateStorage.TryHardLink` 包了 `CreateHardLinkW`，**任何失败都返回 false 而不抛异常**——
跨卷、网络共享、不支持硬链接的文件系统、策略禁止，全部落到同一棵完整且校验过的树，只是更大。
调用方只用它做一个判断（`if (!TryHardLink(...)) { 复制 }`）。没有另做同卷探测：
"失败就复制"已经覆盖了全部原因，而探测本身也会过时。

### 6.4 一次性迁移（未实施）

对已存在的安装，可以直接把现有多棵树的相同文件链接起来，立刻回收空间。
迁移必须是**可选、幂等、失败可忽略**的；已链接的文件再次处理应无副作用。

**不建议现在做**：下一次真实更新本来就会把新版与上一版之间的相同文件链接起来（约 425 MB 的冻结层），
而升级到再下一版时又会把前一版已经链接的那部分一并继承——两三次真实更新之后，存量差异基本自行收敛。
一次性迁移要额外承担"在已知的冻结集合上遍历三棵真实安装树"的风险，收益却在递减。
本机那 1,914 MB 的重复是手工用一个只读脚本量出来的，不值得为它引入一条会删除/改写用户文件的路径。

## 七、风险与边界

| 风险 | 说明 | 处置 |
|---|---|---|
| **未来收紧链接检查会炸，且不可回滚** | 若 `RejectLink` 改成检查链接计数，所有已链接安装启动校验失败，无法远程修复 | 6.2 的语义锁 + 测试 |
| **失去偶然的介质冗余** | 现在同一份 DLL 存在三处不同簇上，一处坏簇不影响另一处；链接后一个坏簇同时打穿所有共享它的版本，连回落链都可能一起失效 | 签名清单会检出损坏并指出重下哪个分片，失败模式是"要重下"而非"不可恢复"。但这是真实退步，需知情接受 |
| **GC 预期要改** | 删一个版本目录不再等于释放其表观体积 | 文档 + 界面文案若涉及 |
| **少数备份/杀软对硬链接处理异常** | 备份软件可能把链接展开成多份 | 低频，接受 |
| 发布侧 | 不受影响：拒绝的是 ReparsePoint；ZIP 无硬链接概念 | — |

## 八、验证判据

实施后至少要有：

1. **单元/集成**（✅ 已满足，2026-10-10）：构造"新版与旧版有相同文件、也有不同文件"的夹具，跑完整
   `PrepareAsync`，断言相同文件的**链接数为 2**（即确实共享同一份字节）、不同文件的链接数为 1；
   随后用签名清单校验整棵树必须通过。链接数用 `GetFileInformationByHandle` 读——共享文件与复制文件的
   大小和摘要完全一样，只有链接数能区分，所以这是唯一不会偶然通过的断言。
2. **空间断言**（以链接数替代）：链接数为 2 即证明只有一份分配。没有另测 `availableBytes`：在同一个
   卷上区分"写了一份"与"没写"需要按簇统计，而那测的是文件系统而不是这段代码。
3. **跨卷回退**（部分满足）：`TryHardLink` 的契约有直接用例（同卷成功且确实建链；源不存在时返回 false
   且不留下文件）。**真正的跨卷场景没有自动化用例**——套件里只有一个卷，造第二个卷需要挂载 VHD，
   代价不成比例。调用方的回退是一条 `if`，已由上面那条契约用例与"源被损坏"用例共同覆盖。
4. **GC**（✅ 已满足，2026-10-10）：`ConfirmHealthyAsync` 后非 Current/Previous 的目录消失；
   `Pending`/`Trial` 指向的目录、以及安装根目录**必须仍在**；残留被占用时提交仍成功。
5. **语义锁回归**（✅ 已满足，2026-10-10）：一棵含硬链接的树必须通过 `ValidateInstalledAsync`
   （启动器真正跑的那套校验），已链接的文件必须能过 `RejectLink`；同时目录联接必须**仍然被拒绝**——
   契约的两半都要钉住，只测一半会让"允许硬链接"退化成一个没人知道边界的例外。

## 九、顺带发现（不在本方案范围内，另行处理）

- **`IMao-Launcher.exe`(65.0) + `KuroSyncBridge.exe`(65.4) = 130.4 MB，占每版 190 MB churn 的 69%。**
  [`ProgramUpdates.md`](ProgramUpdates.md) 写着「稳定启动器本身不原地替换，启动器协议为 `1`」——
  即功能上不随版本变，但字节每次都变（.NET 自包含发布的构建非确定性：时间戳、MVID、依赖顺序）。
  若将来能做到可重现构建或让这两个产物不随程序版本重建，每版落盘可从 ~190 MB 降到 ~60 MB，
  收益比本方案更干净。参见 [`Build_zh-Hans.md`](Build_zh-Hans.md)（可复现构建）。
- **`%LOCALAPPDATA%\IMao-WinUI\ResourceUpdates\staging` 有 69.6 MB 但只有 2 个文件**
  （`dreamzhou-kurotiles.zip` 34.5 + `map-data.zip` 35.1），是资源更新中断后未清理的临时文件。
  [`ResourceUpdates.md`](ResourceUpdates.md) 写着「下载与安装的临时文件由事务结束时清理」，
  所以这是与版本目录孤立副本同类的泄漏，需要单独排查清理路径。
- 玩家侧陈旧材料（本次排查时该机器上实测约 9.6 GB，全部在安装目录之外）：
  回收站里的整个旧安装 3.97 GB、`C:\Dgames\IMao-v2026.10.4.2-windows-x64{,.zip}` 2.58 GB、
  `Downloads\IMAO-v2026.10.7.1-win-x64{,.zip}` 2.58 GB、`C:\Dapps\IMAO-v2026.10.8.1-win-x64.zip` 446.9 MB。
  这些不是程序能管的，但说明"下载包留在原地"是真实存在的玩家行为，
  发行说明里值得加一句"解压后可删除安装包"。

## 十、决策摘要

1. 4 个孤立版本（6.17 GB）是纯泄漏，已清；**GC 已于 2026-10-10 补上**（§6.1）。
2. 剩余三份不是冗余设计失误，而是「运行中 / 回退 / 原始恢复」三角；
   **不要**改成原地替换——稳态磁盘与硬链接方案相同（各 1,959 MB），却要放弃整树校验、
   原子发布、运行文件不被碰、以及跨版本回退。
3. **采纳硬链接复用**（✅ 2026-10-10 已实施）：复用路径改为先建链接、失败则复制，落地后仍逐个校哈希。
   预期回收 1,914 MB（相邻两版之间约 425 MB 的冻结层 + 未变的资源部分），
   保留全部现有保证，网络行为不变。**实机节省量待下一次真实更新实测**——套件只能证明链接确实建立、
   链接数为 2、且共享的树仍能通过启动校验。
4. 配套：版本保留策略（✅ 已实施）、显式允许硬链接的语义锁（✅ 已实施）、同卷回退（✅ 已实施）、
   可选一次性迁移（**不做**，理由见 §6.4）。
5. 新装玩家稳态：**约 1.45 GB**（安装目录 1.28 GB + AppData 0.2 GB），
   对照清理前的 4.06 GB 降 68%。
